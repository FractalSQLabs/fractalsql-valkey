// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
//
// The tool catalog: one tool per FractalSQL command, plus five thin keyspace helpers and a version tool.
//
// It is a table, not a pile of handlers. Each row names the Valkey command, its parameters and how each one
// goes on the wire, and ONE encoder turns a tool call into a command: JSON number arrays become packed
// little-endian float32 blobs, nested arrays become row-major blobs (and supply the `dim` the command wants),
// integer arrays become packed uint32, hex strings become raw bytes. ONE decoder turns the reply back into
// JSON. A command is therefore added by adding a row, and the argument grammar lives in exactly one place.
#include "commands.h"
#include "../version.h"
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <deque>
#include <random>
#include "../vectors.h"
#include "../logging.h"

namespace fsqlmcp
{
namespace
{
    enum class K
    {
        Key,        // a Valkey key name (string)
        Str,        // free text, sent as given
        Int,        // integer, sent as decimal text
        Num,        // number, sent as shortest round-trip decimal text
        Vec,        // array of numbers -> packed little-endian float32
        Mat,        // array of equal-length arrays -> row-major float32; fixes `dim`
        UInts,      // array of non-negative integers -> packed little-endian uint32
        Obj,        // JSON object -> its JSON text
        JsonText,   // a JSON document -> its text; published as a string
        Bool,       // true/false (custom tools only)
        Filter,     // {hash, value} -> the two tokens of a FILTER clause
        Hex,        // hex string -> raw bytes
        HexList,    // array of equal-length hex strings -> concatenated bytes; fixes `n_bytes`
        DimOf,      // (hidden) the row width of the Mat named in `ref`
        BytesOf     // (hidden) the per-item byte length of the HexList named in `ref`
    };

    enum class Out { Auto, Number, Vector, Hex, Text };

    struct Prm
    {
        std::string name;
        K           kind = K::Str;
        bool        req = true;
        std::string desc;
        std::string kw;                 // non-empty: sent as KEYWORD value, after the positional arguments
        Json        def = nullptr;      // sent in place of a skipped optional positional argument
        std::string ref;                // DimOf / BytesOf: the parameter it measures
        int         width = 0;          // Mat: required row width (0 = any)
        std::vector<std::string> oneOf; // allowed values (case-insensitive), empty = any
        bool        fromContext = false; // the caller's own state stands in when the caller names none

        Prm&& O( Json d = nullptr ) && { req = false; def = std::move( d ); return std::move( *this ); }
        Prm&& KW( std::string k ) && { kw = std::move( k ); req = false; return std::move( *this ); }
        Prm&& W( int w ) && { width = w; return std::move( *this ); }
        Prm&& OneOf( std::vector<std::string> v ) && { oneOf = std::move( v ); return std::move( *this ); }
        // Optional: the caller's own state stands in for it when the caller
        // does not name one.
        Prm&& Ctx() && { req = false; fromContext = true; return std::move( *this ); }
    };

    Prm P( const char* n, K k, const char* d ) { Prm p; p.name = n; p.kind = k; p.desc = d; return p; }
    Prm Key( const char* n, const char* d )   { return P( n, K::Key, d ); }
    Prm Str( const char* n, const char* d )   { return P( n, K::Str, d ); }
    Prm Int( const char* n, const char* d )   { return P( n, K::Int, d ); }
    Prm Num( const char* n, const char* d )   { return P( n, K::Num, d ); }
    Prm Vec( const char* n, const char* d )   { return P( n, K::Vec, d ); }
    Prm Mat( const char* n, const char* d )   { return P( n, K::Mat, d ); }
    Prm UInts( const char* n, const char* d ) { return P( n, K::UInts, d ); }
    Prm Obj( const char* n, const char* d )   { return P( n, K::Obj, d ); }
    // A JSON document carried as text: the value may be given as a JSON
    // object or as its text, and it is published as a string.
    Prm JsonText( const char* n, const char* d ) { return P( n, K::JsonText, d ); }
    Prm Bool( const char* n, const char* d )  { return P( n, K::Bool, d ); }
    Prm Filter( const char* n, const char* d ) { return P( n, K::Filter, d ); }
    Prm Hex( const char* n, const char* d )   { return P( n, K::Hex, d ); }
    Prm HexList( const char* n, const char* d ) { return P( n, K::HexList, d ); }
    Prm DimOf( const char* ref )   { Prm p; p.name = std::string( "dim_of_" ) + ref; p.kind = K::DimOf; p.ref = ref; return p; }
    Prm BytesOf( const char* ref ) { Prm p; p.name = std::string( "bytes_of_" ) + ref; p.kind = K::BytesOf; p.ref = ref; return p; }

    struct Tool
    {
        std::string name, category, command, desc;
        ToolClass   cls = ToolClass::Cheap;
        bool        readOnly = true, destructive = false, openWorld = false, secret = false;
        Out         out = Out::Auto;
        std::vector<Prm> params;
        // The module's own text for a failure this tool can recognise; not used for control flow.
        std::function<Json( Db&, const Tool&, const Json&, CallCtx& )> custom;   // replaces the table encoder
    };

    // ------------------------------------------------------------------ encoding
    constexpr std::size_t kMaxElems = 4u << 20;     // 16 MiB of float32 in one argument, the module's own bound

    double finiteNumber( const Json& raw, const std::string& what )
    {
        const Json e = vectors::coerceNumber( raw );
        if( !e.is_number() || e.is_boolean() || !std::isfinite( e.get<double>() ) )
            throw ToolError( ErrorClass::Schema, what + " contains a non-numeric or non-finite element: " + raw.dump().substr( 0, 40 ) );
        return e.get<double>();
    }

    void appendF32( std::string& out, double v )
    {
        const float f = static_cast<float>( v );
        if( !std::isfinite( f ) ) throw ToolError( ErrorClass::Schema, "a value is out of float32 range" );
        char b[ 4 ]; std::memcpy( b, &f, 4 );    // little-endian hosts only (x86-64, arm64)
        out.append( b, 4 );
    }

    std::string packVec( const Json& v, const std::string& name )
    {
        if( !v.is_array() ) throw ToolError( ErrorClass::Schema, name + " must be a JSON array of numbers" );
        if( v.empty() ) throw ToolError( ErrorClass::Schema, name + " must not be an empty array" );
        if( v.size() > kMaxElems ) throw ToolError( ErrorClass::Schema, name + " has too many elements" );
        std::string out; out.reserve( v.size() * 4 );
        for( const Json& e : v ) appendF32( out, finiteNumber( e, name ) );
        return out;
    }

    // Nested rows, every row the same width. `width` is set to the row width.
    std::string packMat( const Json& m, const std::string& name, int requiredWidth, std::size_t& width )
    {
        if( !m.is_array() || m.empty() ) throw ToolError( ErrorClass::Schema, name + " must be a non-empty JSON array" );
        width = 0;
        std::string out;
        std::size_t total = 0;
        for( std::size_t r = 0; r < m.size(); ++r )
        {
            const Json& row = m[ r ];
            if( !row.is_array() || row.empty() )
                throw ToolError( ErrorClass::Schema, name + " row " + std::to_string( r ) + " must be a non-empty array of numbers" );
            if( r == 0 ) width = row.size();
            else if( row.size() != width )
                throw ToolError( ErrorClass::Schema, name + " row " + std::to_string( r ) + " has " + std::to_string( row.size() )
                                 + " values, expected " + std::to_string( width ) );
            if( requiredWidth > 0 && row.size() != static_cast<std::size_t>( requiredWidth ) )
                throw ToolError( ErrorClass::Schema, name + " rows must each have exactly " + std::to_string( requiredWidth ) + " values" );
            total += row.size();
            if( total > kMaxElems ) throw ToolError( ErrorClass::Schema, name + " has too many elements" );
            for( const Json& e : row ) appendF32( out, finiteNumber( e, name ) );
        }
        return out;
    }

    std::string packUInts( const Json& v, const std::string& name )
    {
        if( !v.is_array() || v.empty() ) throw ToolError( ErrorClass::Schema, name + " must be a non-empty JSON array of integers" );
        if( v.size() > kMaxElems ) throw ToolError( ErrorClass::Schema, name + " has too many elements" );
        std::string out; out.reserve( v.size() * 4 );
        for( const Json& e : v )
        {
            if( !e.is_number_integer() || e.is_boolean() || e.get<long long>() < 0 || e.get<long long>() > 4294967295LL )
                throw ToolError( ErrorClass::Schema, name + " must contain integers from 0 to 4294967295" );
            const std::uint32_t u = static_cast<std::uint32_t>( e.get<long long>() );
            char b[ 4 ]; std::memcpy( b, &u, 4 );
            out.append( b, 4 );
        }
        return out;
    }

    std::string hexToBytes( const std::string& h, const std::string& name )
    {
        if( h.size() % 2 != 0 || h.empty() ) throw ToolError( ErrorClass::Schema, name + " must be a non-empty hex string of even length" );
        auto nib = [ & ]( char c ) -> int
        {
            if( c >= '0' && c <= '9' ) return c - '0';
            if( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
            if( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
            throw ToolError( ErrorClass::Schema, name + " is not valid hex" );
        };
        std::string out;
        for( std::size_t i = 0; i < h.size(); i += 2 ) out.push_back( static_cast<char>( ( nib( h[ i ] ) << 4 ) | nib( h[ i + 1 ] ) ) );
        return out;
    }

    std::string numText( double v )
    {
        char buf[ 40 ];
        std::snprintf( buf, sizeof( buf ), "%.17g", v );
        return buf;
    }

    std::string lower( std::string s ) { for( char& c : s ) c = static_cast<char>( std::tolower( static_cast<unsigned char>( c ) ) ); return s; }

    // Encodes one provided parameter. `derived` carries the widths fixed by earlier Mat / HexList parameters.
    std::string encodeOne( const Prm& p, const Json& v, std::map<std::string, std::size_t>& derived )
    {
        switch( p.kind )
        {
            case K::Key:
                if( v.get<std::string>().empty() ) throw ToolError( ErrorClass::Schema, p.name + " must not be empty" );
                return v.get<std::string>();
            case K::Str:
            {
                std::string s = v.get<std::string>();
                if( !p.oneOf.empty() )
                {
                    bool ok = false;
                    for( const std::string& o : p.oneOf ) if( lower( o ) == lower( s ) ) { ok = true; s = o; break; }
                    if( !ok )
                    {
                        std::string list; for( const std::string& o : p.oneOf ) list += ( list.empty() ? "" : ", " ) + o;
                        throw ToolError( ErrorClass::Schema, p.name + " must be one of: " + list );
                    }
                }
                return s;
            }
            case K::Int:  return std::to_string( v.get<long long>() );
            case K::Num:
            {
                const double d = v.get<double>();
                if( !std::isfinite( d ) ) throw ToolError( ErrorClass::Schema, p.name + " must be finite" );
                return numText( d );
            }
            case K::Vec:   return packVec( v, p.name );
            case K::Mat:
            {
                std::size_t w = 0;
                std::string blob = packMat( v, p.name, p.width, w );
                derived[ p.name ] = w;
                return blob;
            }
            case K::UInts: return packUInts( v, p.name );
            case K::Obj:   return v.dump();
            case K::JsonText: return v.is_string() ? v.get<std::string>() : v.dump();
            case K::Bool:  return v.get<bool>() ? "1" : "0";
            case K::Filter:
            {
                if( !v.is_object() || !v.contains( "hash" ) || !v.contains( "value" ) || !v.at( "hash" ).is_string() || !v.at( "value" ).is_string()
                    || v.at( "hash" ).get<std::string>().empty() )
                    throw ToolError( ErrorClass::Schema, p.name + " must be an object {\"hash\": <hash key>, \"value\": <string>}" );
                return v.at( "hash" ).get<std::string>() + '\x1f' + v.at( "value" ).get<std::string>();
            }
            case K::Hex:   return hexToBytes( v.get<std::string>(), p.name );
            case K::HexList:
            {
                if( !v.is_array() || v.empty() ) throw ToolError( ErrorClass::Schema, p.name + " must be a non-empty array of hex strings" );
                std::string all; std::size_t each = 0;
                for( std::size_t i = 0; i < v.size(); ++i )
                {
                    if( !v[ i ].is_string() ) throw ToolError( ErrorClass::Schema, p.name + " must contain only hex strings" );
                    const std::string b = hexToBytes( v[ i ].get<std::string>(), p.name );
                    if( i == 0 ) each = b.size();
                    else if( b.size() != each ) throw ToolError( ErrorClass::Schema, p.name + " items must all have the same length" );
                    all += b;
                }
                derived[ p.name ] = each;
                return all;
            }
            case K::DimOf:
            case K::BytesOf:
            {
                auto it = derived.find( p.ref );
                if( it == derived.end() || it->second == 0 )
                    throw ToolError( ErrorClass::Schema, p.ref + " must be given as an array of equal-length rows" );
                return std::to_string( it->second );
            }
        }
        return {};
    }

    JsonType jsonTypeOf( K k )
    {
        switch( k )
        {
            case K::Key: case K::Str: case K::Hex: case K::JsonText: return JsonType::String;
            case K::Int: case K::DimOf: case K::BytesOf: return JsonType::Integer;
            case K::Num: return JsonType::Number;
            case K::Obj: case K::Filter: return JsonType::Object;
            case K::Bool: return JsonType::Boolean;
            default: return JsonType::Array;
        }
    }

    // The element kind of an array parameter: what the elements of the array
    // are. It is advertised in the tool's input schema and checked when the
    // call arrives, so the advertised items type is a promise the server
    // keeps.
    JsonType itemTypeOf( K k )
    {
        switch( k )
        {
            case K::Vec: return JsonType::Number;      // a vector is a list of numbers
            case K::Mat: return JsonType::Array;        // a matrix is a list of rows
            case K::UInts: return JsonType::Integer;    // indices are whole numbers
            case K::HexList: return JsonType::String;   // fingerprints are hex strings
            default: return JsonType::Number;
        }
    }

    // ------------------------------------------------------------------ decoding
    Json parseNumberOrText( const std::string& s )
    {
        if( !s.empty() )
        {
            char* end = nullptr;
            const double d = std::strtod( s.c_str(), &end );
            if( *end == '\0' && std::isfinite( d ) && ( std::isdigit( static_cast<unsigned char>( s[ 0 ] ) ) || s[ 0 ] == '-' || s[ 0 ] == '+' || s[ 0 ] == '.' ) )
                return d;
        }
        return s;
    }

    Json blobToFloats( const std::string& b )
    {
        if( b.size() % 4 != 0 ) throw ToolError( ErrorClass::ServerInternal, "a vector reply is not a whole number of float32 values" );
        Json arr = Json::array();
        for( std::size_t i = 0; i < b.size(); i += 4 )
        {
            float f; std::memcpy( &f, b.data() + i, 4 );
            arr.push_back( std::round( static_cast<double>( f ) * 1e7 ) / 1e7 );
        }
        return arr;
    }

    std::string toHex( const std::string& b )
    {
        static const char* d = "0123456789abcdef";
        std::string out;
        for( unsigned char c : b ) { out.push_back( d[ c >> 4 ] ); out.push_back( d[ c & 15 ] ); }
        return out;
    }

    Json decodeAuto( const Resp& r )
    {
        switch( r.type )
        {
            case Resp::Type::Nil:     return nullptr;
            case Resp::Type::Integer: return r.i;
            case Resp::Type::Simple:  return r.str == "OK" ? Json( true ) : Json( r.str );
            case Resp::Type::Bulk:
            {
                if( !r.str.empty() && ( r.str[ 0 ] == '{' || r.str[ 0 ] == '[' ) )
                    try { return Json::parse( r.str ); } catch( ... ) {}
                return parseNumberOrText( r.str );
            }
            case Resp::Type::Array:
            {
                Json a = Json::array();
                for( const Resp& it : r.items ) a.push_back( decodeAuto( it ) );
                return a;
            }
            case Resp::Type::Error: break;
        }
        return nullptr;
    }

    Json wrapResult( Json v, const char* key = "value" )
    {
        if( v.is_object() ) return v;
        return Json{ { key, std::move( v ) } };
    }

    Json decodeReply( const Resp& r, Out out )
    {
        switch( out )
        {
            case Out::Vector:
                if( r.type != Resp::Type::Bulk ) return wrapResult( decodeAuto( r ) );
                return Json{ { "vector", blobToFloats( r.str ) }, { "dim", r.str.size() / 4 } };
            case Out::Hex:
                if( r.type != Resp::Type::Bulk ) return wrapResult( decodeAuto( r ) );
                return Json{ { "hex", toHex( r.str ) }, { "bytes", r.str.size() } };
            case Out::Text:
                return Json{ { "text", r.str } };
            case Out::Number:
            {
                Json v = decodeAuto( r );
                return wrapResult( v );
            }
            case Out::Auto: break;
        }
        Json v = decodeAuto( r );
        if( v.is_array() ) return Json{ { "items", v } };
        return wrapResult( v );
    }

    // ------------------------------------------------------------------ the generic handler
    //
    // The state a call acts on when it does not name one: the caller's own
    // context, named after the handle it holds. The name is the handle the
    // caller was given, so a session the caller never named is still one it
    // can see in the session list and can address by name.
    std::string contextSessionName( const CallCtx& ctx )
    {
        if( ctx.contextKey.empty() )
            throw ToolError( ErrorClass::Schema,
                "this call has no context of its own: name the session to act on" );
        std::string out = "ctx-";
        for( char c : ctx.contextKey )
        {
            if( out.size() >= 60 ) break;
            if( isalnum( static_cast<unsigned char>( c ) ) || c == '_' || c == '.' || c == ':' || c == '-' )
                out.push_back( c );
        }
        return out;
    }

    Json runTable( Db& db, const Tool& t, const Json& args, const CallCtx& ctx )
    {
        std::vector<std::string> cmd;
        { std::size_t s = 0; for( ;; ) { const std::size_t sp = t.command.find( ' ', s ); cmd.push_back( t.command.substr( s, sp == std::string::npos ? sp : sp - s ) ); if( sp == std::string::npos ) break; s = sp + 1; } }

        std::map<std::string, std::size_t> derived;
        std::map<std::string, std::string> encoded;    // positional values, encoded once, in table order
        // 1) encode every provided non-hidden parameter (this also fixes the derived widths)
        for( const Prm& p : t.params )
        {
            if( p.kind == K::DimOf || p.kind == K::BytesOf ) continue;
            if( args.contains( p.name ) ) encoded[ p.name ] = encodeOne( p, args.at( p.name ), derived );
        }
        // 2) positional arguments, in order, up to the last one the caller actually supplied
        int lastProvided = -1;
        for( std::size_t i = 0; i < t.params.size(); ++i )
        {
            const Prm& p = t.params[ i ];
            if( !p.kw.empty() ) continue;
            if( p.kind == K::DimOf || p.kind == K::BytesOf || args.contains( p.name ) || p.fromContext )
                lastProvided = static_cast<int>( i );
        }
        std::string firstGap;
        std::string ownSession;   // the caller's own state, when the caller named none
        for( int i = 0; i <= lastProvided; ++i )
        {
            const Prm& p = t.params[ static_cast<std::size_t>( i ) ];
            if( !p.kw.empty() ) continue;
            if( p.kind == K::DimOf || p.kind == K::BytesOf ) { cmd.push_back( encodeOne( p, Json(), derived ) ); continue; }
            auto it = encoded.find( p.name );
            if( it != encoded.end() ) { cmd.push_back( it->second ); continue; }
            if( p.fromContext )
            {
                ownSession = contextSessionName( ctx );
                cmd.push_back( ownSession );
                continue;
            }
            if( p.req ) throw ToolError( ErrorClass::Schema, "missing required argument: " + p.name );
            if( p.def.is_null() )
                throw ToolError( ErrorClass::Schema, p.name + " is required when a later optional argument is given" );
            Prm tmp = p;
            cmd.push_back( encodeOne( tmp, p.def, derived ) );
        }
        // 3) keyword arguments
        for( const Prm& p : t.params )
        {
            if( p.kw.empty() ) continue;
            auto it = encoded.find( p.name );
            if( it == encoded.end() ) continue;
            cmd.push_back( p.kw );
            if( p.kind == K::Filter )
            {
                const std::size_t cut = it->second.find( '\x1f' );
                cmd.push_back( it->second.substr( 0, cut ) );
                cmd.push_back( it->second.substr( cut + 1 ) );
            }
            else cmd.push_back( it->second );
        }
        try
        {
            return decodeReply( db.call( cmd ), t.out );
        }
        catch( const ToolError& e )
        {
            // The engine creates a session the first time something writes to
            // it, so a read of the caller's own state can arrive before the
            // session exists. Create it and read it again.
            if( ownSession.empty() || e.message.find( "no such session" ) == std::string::npos ) throw;
            db.call( { "FRACTAL.DIVERSIFY.PARAMS", ownSession } );
            return decodeReply( db.call( cmd ), t.out );
        }
    }

    // ------------------------------------------------------------------ keyspace helpers
    std::string g_keyPrefix;      // FSQL_MCP_KEY_PREFIX, read once

    void checkKey( const std::string& key )
    {
        if( key.empty() ) throw ToolError( ErrorClass::Schema, "key must not be empty" );
        if( !g_keyPrefix.empty() && key.rfind( g_keyPrefix, 0 ) != 0 )
            throw ToolError( ErrorClass::Schema, "key '" + key + "' is outside the allowed prefix '" + g_keyPrefix
                             + "' (FSQL_MCP_KEY_PREFIX); this tool only writes under it" );
    }

    // Every MCP client authenticates to Valkey as the same ACL user, so the keyspace itself
    // cannot tell clients apart. The server therefore records which MCP client wrote each key in a hash
    // (outside the caller-controlled prefix) and refuses a write or delete by a different client.
    // Keys with no recorded owner (written by something other than this server) stay open, as before.
    const char* const kOwnerHash = "fsqlmcp:owners";

    void checkOwnerAndClaim( Db& db, const std::string& key, const std::string& client )
    {
        // FSQL_MCP_KEY_OWNERSHIP=off for deployments where one team deliberately shares keys across sessions.
        static const bool enabled = envBoolOr( "FSQL_MCP_KEY_OWNERSHIP", true );
        if( !enabled ) return;
        Resp r = db.call( { "HGET", kOwnerHash, key } );
        if( !r.isNil() && r.isString() && r.str != client )
            throw ToolError( ErrorClass::Schema, "key '" + key + "' was written by another MCP client; this client may not overwrite or delete it" );
        if( r.isNil() ) db.call( { "HSETNX", kOwnerHash, key, client } );
    }

    std::string tempKey()
    {
        static std::atomic<unsigned long long> n{ 0 };
        static const unsigned long long salt = std::random_device{}();
        return "fsqlmcp:tmp:" + std::to_string( salt ) + ":" + std::to_string( ++n );
    }

    // FRACTAL.SEARCH / FRACTAL.EXPLORE with either a stored corpus key or an inline corpus. An inline corpus is
    // written to a short-lived scratch key, searched, and deleted again, so the tool works on data the caller
    // has in hand exactly as the SQL editions' inline-corpus form does.
    Json searchHandler( Db& db, const Tool& t, const Json& args )
    {
        const bool hasKey = args.contains( "corpus_key" ), hasInline = args.contains( "corpus" );
        if( hasKey == hasInline )
            throw ToolError( ErrorClass::Schema, "give exactly one of corpus_key (a stored corpus) or corpus (an inline array of rows)" );
        std::string key; bool scratch = false;
        if( hasKey ) { key = args.at( "corpus_key" ).get<std::string>(); if( key.empty() ) throw ToolError( ErrorClass::Schema, "corpus_key must not be empty" ); }
        else
        {
            std::size_t w = 0;
            const std::string blob = packMat( args.at( "corpus" ), "corpus", 0, w );
            if( w == 0 ) throw ToolError( ErrorClass::Schema, "corpus must be an array of equal-length rows" );
            key = tempKey(); scratch = true;
            db.call( { "SET", key, blob, "PX", "120000" } );
        }
        struct Cleanup { Db& db; const std::string& key; bool on; ~Cleanup() { if( on ) { try { db.call( { "DEL", key } ); } catch( ... ) {} } } } cleanup{ db, key, scratch };

        std::vector<std::string> cmd{ t.command, key, packVec( args.at( "query" ), "query" ) };
        if( t.name == "fractal_search" )
            cmd.push_back( std::to_string( args.value( "k", 5LL ) ) );
        else if( args.contains( "population_size" ) )
            cmd.push_back( std::to_string( args.at( "population_size" ).get<long long>() ) );
        if( args.contains( "session" ) ) { cmd.push_back( "SESSION" ); cmd.push_back( args.at( "session" ).get<std::string>() ); }
        return decodeReply( db.call( cmd ), Out::Auto );
    }

    Json storeVectors( Db& db, const Tool&, const Json& args, const std::string& client )
    {
        const std::string key = args.at( "key" ).get<std::string>(); checkKey( key );
        checkOwnerAndClaim( db, key, client );
        std::size_t w = 0;
        const std::string blob = packMat( args.at( "rows" ), "rows", 0, w );
        if( w == 0 ) throw ToolError( ErrorClass::Schema, "rows must be an array of equal-length rows" );
        if( !args.value( "overwrite", true ) )
        {
            Resp r = db.call( { "EXISTS", key } );
            if( r.i != 0 ) throw ToolError( ErrorClass::Engine, "key '" + key + "' already exists and overwrite is false" );
        }
        db.call( { "SET", key, blob } );
        return Json{ { "key", key }, { "rows", blob.size() / 4 / w }, { "dim", w }, { "bytes", blob.size() } };
    }

    Json storeSeries( Db& db, const Tool&, const Json& args, const std::string& client )
    {
        const std::string key = args.at( "key" ).get<std::string>(); checkKey( key );
        checkOwnerAndClaim( db, key, client );
        const std::string blob = packVec( args.at( "values" ), "values" );
        if( !args.value( "overwrite", true ) )
        {
            Resp r = db.call( { "EXISTS", key } );
            if( r.i != 0 ) throw ToolError( ErrorClass::Engine, "key '" + key + "' already exists and overwrite is false" );
        }
        db.call( { "SET", key, blob } );
        return Json{ { "key", key }, { "samples", blob.size() / 4 }, { "bytes", blob.size() } };
    }

    Json storeHash( Db& db, const Tool&, const Json& args, const std::string& client )
    {
        const std::string key = args.at( "key" ).get<std::string>(); checkKey( key );
        checkOwnerAndClaim( db, key, client );
        const Json& f = args.at( "fields" );
        if( f.empty() ) throw ToolError( ErrorClass::Schema, "fields must not be empty" );
        if( f.size() > 100000 ) throw ToolError( ErrorClass::Schema, "fields has too many entries (at most 100000)" );
        std::vector<std::string> cmd{ "HSET", key };
        for( auto it = f.begin(); it != f.end(); ++it )
        {
            cmd.push_back( it.key() );
            if( it.value().is_string() ) cmd.push_back( it.value().get<std::string>() );
            else if( it.value().is_number() || it.value().is_boolean() ) cmd.push_back( it.value().dump() );
            else throw ToolError( ErrorClass::Schema, "fields values must be strings or numbers (field '" + it.key() + "')" );
        }
        Resp r = db.call( cmd );
        return Json{ { "key", key }, { "fields_added", r.i }, { "fields_given", f.size() } };
    }

    Json deleteKey( Db& db, const Tool&, const Json& args, const std::string& client )
    {
        const std::string key = args.at( "key" ).get<std::string>(); checkKey( key );
        checkOwnerAndClaim( db, key, client );
        Resp r = db.call( { "DEL", key } );
        db.call( { "HDEL", kOwnerHash, key } );
        return Json{ { "key", key }, { "deleted", r.i } };
    }

    Json keyInfo( Db& db, const Tool&, const Json& args )
    {
        const std::string key = args.at( "key" ).get<std::string>();
        if( key.empty() ) throw ToolError( ErrorClass::Schema, "key must not be empty" );
        Resp ty = db.call( { "TYPE", key } );
        Json out{ { "key", key }, { "type", ty.str } };
        if( ty.str == "string" )
        {
            Resp n = db.call( { "STRLEN", key } );
            out[ "bytes" ] = n.i;
            out[ "float32_values" ] = n.i / 4;
            out[ "whole_float32_values" ] = n.i % 4 == 0;
        }
        else if( ty.str == "hash" ) out[ "fields" ] = db.call( { "HLEN", key } ).i;
        return out;
    }

    Json versionTool( Db& db, const Tool&, const Json& )
    {
        Json out{ { "plugin", kMcpPluginName }, { "plugin_version", FSQL_MCP_VERSION } };
        out[ "module_version" ] = decodeAuto( db.call( { "FRACTAL.VERSION" } ) );
        try { out[ "edition" ] = decodeAuto( db.call( { "FRACTAL.EDITION" } ) ); } catch( const ToolError& ) { out[ "edition" ] = nullptr; }
        try
        {
            Resp info = db.call( { "INFO", "server" } );
            std::size_t pos = info.str.find( "valkey_version:" );
            if( pos == std::string::npos ) pos = info.str.find( "redis_version:" );
            if( pos != std::string::npos )
            {
                const std::size_t s = info.str.find( ':', pos ) + 1, e = info.str.find_first_of( "\r\n", s );
                out[ "server_version" ] = info.str.substr( s, e - s );
            }
        }
        catch( const ToolError& ) {}
        return out;
    }

    // ------------------------------------------------------------------ the table
    std::deque<Tool>& table()
    {
        static std::deque<Tool> t = []
        {
            std::deque<Tool> v;
            auto add = [ & ]( Tool x ) { v.push_back( std::move( x ) ); };
            // Per-tool annotation overrides. The rule the whole catalogue
            // follows: destructive = may change database, ledger or session
            // state; readOnly = writes nothing at all; secret = the arguments
            // or the result may carry content a client should not echo or log
            // casually (an LLM prompt, an embedding, a provider endpoint).
            // The defaults above are the fail-safe ones; this table is where
            // a tool's real behaviour is recorded.
            struct Annotation
            {
                const char* name;
                bool        readOnly;
                bool        destructive;
                bool        secret;
            };
            static const Annotation kAnnotations[] = {
                // The reasoning and embedding tools reach a model: they are
                // not read-only (a model call is an effect) and their
                // arguments carry the caller's content.
                { "fractal_reason",              false, false, true  },
                { "fractal_embed",               false, false, true  },
                // The agents that reach a model carry the caller's content
                // into it, so they are secret. The pure retrieval ones do
                // not reach a model and are read-only.
                { "fractal_agent_allocate",             false, false, true  },
                { "fractal_agent_anomaly_triage",       false, false, true  },
                { "fractal_agent_data_analyst",         false, true,  true  },
                { "fractal_agent_detour_classify",      false, false, true  },
                { "fractal_agent_diverse_portfolios",   false, false, true  },
                { "fractal_agent_network_coverage_alert",false, false, true  },
                { "fractal_agent_outlier_intercept",    false, false, true  },
                { "fractal_agent_patient_deterioration_triage", false, false, true },
                { "fractal_agent_rebalance_sibling",    false, false, true  },
                { "fractal_agent_regime_triage",        false, false, true  },
                { "fractal_agent_route_task",           false, false, true  },
                { "fractal_agent_schedule_workload",    false, false, true  },
                { "fractal_agent_track_anomaly",        false, false, true  },
                // Pure retrieval: no model, nothing written.
                { "fractal_agent_recommend_diverse",    true,  false, false },
                { "fractal_agent_recall_hybrid",        true,  false, false },
                { "fractal_agent_detect_loop",          true,  false, false },
                { "fractal_agent_trajectory_predict",   true,  false, false },
                { "fractal_agent_plan_explore",         false, false, false },
                // The two text-driven agents reach a model: not read-only, and
                // the question they carry is the caller's content.
                { "fractal_search_agent",                false, false, true  },
                { "fractal_rag_agent",                   false, false, true  },
                // The searches read a corpus and write nothing.
                { "fractal_search",                     true,  false, false },
                { "fractal_search_explore",             true,  false, false },
            };
            auto tool = [ & ]( const char* name, const char* cat, const char* cmd, ToolClass cls, const char* desc, std::vector<Prm> params,
                               bool ro = true, int destructive = -1, bool openWorld = false, Out out = Out::Auto )
            {
                Tool x; x.name = name; x.category = cat; x.command = cmd; x.cls = cls; x.desc = desc; x.params = std::move( params );
                x.readOnly = ro; x.openWorld = openWorld; x.out = out;
                // Fail-safe: a tool that is not read-only and did not state destructive=false is destructive.
                x.destructive = ( destructive < 0 ) ? !ro : ( destructive != 0 );
                return x;
            };
            const ToolClass C = ToolClass::Cheap, E = ToolClass::Expensive;
            const char* kSession = "Optional diversify session name: the search avoids what that session has rejected (fractal_diversify_enable first). Makes the call stateful.";
            // The session a state tool acts on when the caller names none: the
            // caller's own session, which no other caller can reach.
            const char* kOwnSession = "The session name. Omit it to act on your own session: the one this context owns.";

            // ---- discovery ----
            { Tool x = tool( "fractal_search", "discovery", "FRACTAL.SEARCH", E,
                "Sniper-mode Stochastic Fractal Search: the K nearest rows of a corpus to a query, minimizing cosine distance. "
                "Give corpus_key (a stored corpus: rows packed as float32 in one key, see fractal_store_vectors) OR corpus (an inline array of rows). "
                "Returns best_point, best_fit and top_k as [{idx, dist}].",
                { Key( "corpus_key", "A stored corpus key." ).O(), Mat( "corpus", "An inline corpus: an array of equal-length rows." ).O(),
                  Vec( "query", "The query vector; its length is the corpus dimension." ),
                  Int( "k", "How many rows to return." ).O( 5 ), Str( "session", kSession ).KW( "SESSION" ) },
                false, 0 );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& ) { return searchHandler( db, t, a ); }; add( std::move( x ) ); }
            { Tool x = tool( "fractal_search_explore", "discovery", "FRACTAL.EXPLORE", E,
                "Scout-mode SFS: the whole converged population (every particle's position and fitness) rather than only the top K, "
                "for clustering and diversity analysis. Same corpus_key / corpus choice as fractal_search.",
                { Key( "corpus_key", "A stored corpus key." ).O(), Mat( "corpus", "An inline corpus: an array of equal-length rows." ).O(),
                  Vec( "query", "The query vector." ), Int( "population_size", "Particles in the search." ).O(),
                  Str( "session", kSession ).KW( "SESSION" ) },
                false, 0 );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& ) { return searchHandler( db, t, a ); }; add( std::move( x ) ); }
            add( tool( "fractal_diversify_enable", "discovery", "FRACTAL.DIVERSIFY.ENABLE", C,
                "Start (or reset) a diversify session: later searches given the same session avoid the neighbourhoods it has been told to reject.",
                { Key( "session", kOwnSession ).Ctx() }, false ) );
            add( tool( "fractal_diversify_disable", "discovery", "FRACTAL.DIVERSIFY.DISABLE", C,
                "Turn diversification off for a session (the session and its history stay).", { Key( "session", kOwnSession ).Ctx() }, false ) );
            add( tool( "fractal_diversify_params", "discovery", "FRACTAL.DIVERSIFY.PARAMS", C,
                "Read a session's diversify parameters, or set some of them with a JSON object (window_n, stall_threshold, repulsion_sigma, repulsion_weight, max_shadows_considered, tail_buffer_cap).",
                { Key( "session", kOwnSession ).Ctx(), Obj( "params", "Fields to change; omit to just read." ).O() }, false ) );
            add( tool( "fractal_explain_result", "discovery", "FRACTAL.DIVERSIFY.EXPLAIN", C,
                "Why the last search through a session looked the way it did: D_q, whether diversify is on, and overhead.", { Key( "session", kOwnSession ).Ctx() } ) );
            add( tool( "fractal_feedback_report", "discovery", "FRACTAL.FEEDBACK", C,
                "Tell a session how a result was received so later searches steer away from rejected rows.",
                { Key( "session", kOwnSession ).Ctx(), Int( "result_handle", "The row index (scan position) of the result." ),
                  Str( "kind", "The reaction." ).OneOf( { "positive", "negative", "dwell" } ), Int( "dwell_ms", "For kind dwell: how long the user stayed." ).O() }, false ) );
            add( tool( "fractal_isolate_background", "discovery", "FRACTAL.ISOLATE_BACKGROUND", C,
                "Mark one result as background noise for a session so it is excluded from future repulsion.",
                { Key( "session", kOwnSession ).Ctx(), Int( "result_handle", "The row index of the result." ) }, false ) );
            add( tool( "fractal_session_close", "discovery", "FRACTAL.SESSION.CLOSE", C,
                "Drop a diversify session and free its memory.", { Key( "session", kOwnSession ).Ctx() }, false, true ) );
            add( tool( "fractal_session_list", "discovery", "FRACTAL.SESSION.LIST", C, "List the live diversify sessions.", {} ) );

            // ---- cognition ----
            add( tool( "fractal_reason", "cognition", "FRACTAL.REASON", E,
                "Send a question (and optional JSON context) to the configured reasoning model through the fractalsqld daemon. "
                "Needs a daemon with a model; otherwise a clean 'not configured' error.",
                { Str( "query", "The question." ), JsonText( "context", "Optional JSON context for the model." ).O( "{}" ) }, false, false, true ) );
            add( tool( "fractal_embed", "cognition", "FRACTAL.EMBED", E,
                "Embed text with the configured embedding model through the fractalsqld daemon; returns the vector.",
                { Str( "input", "The text to embed." ) }, true, false, true, Out::Vector ) );

            // ---- agency ----
            auto agent = [ & ]( const char* name, const char* cmd, const char* desc, std::vector<Prm> ps, bool llm = true, bool ro = true )
            {
                add( tool( name, "agency", cmd, E, desc, std::move( ps ), ro, false, llm ) );
            };
            const char* kIds = "Optional hash key mapping row index to a row id.";
            const char* kContent = "Optional hash key mapping row index to its content.";
            const char* kCtx = "Optional JSON object handed to the reasoner as context.";
            agent( "fractal_agent_anomaly_triage", "FRACTAL.AGENT.ANOMALY_TRIAGE",
                "Drift on one entity's metric series, then a model-written triage summary. Returns threat_score, anomaly_type and triage_summary (or the computed half plus reasoning_step_failed when no model answers).",
                { Key( "series_key", "Key holding the series as packed float32, oldest first." ), Int( "baseline_window", "Baseline window (16 to 16777216)." ).O( 32 ) } );
            agent( "fractal_agent_allocate", "FRACTAL.AGENT.ALLOCATE",
                "Cardinality-constrained portfolio optimisation, then a written rationale. Returns the allocation and its Sharpe ratio.",
                { Vec( "mu", "Expected returns, n values." ), Mat( "cov", "The n x n covariance matrix, as nested rows." ),
                  Int( "cardinality", "At most this many assets carry weight (1 to n)." ), JsonText( "context", kCtx ).O( "{}" ) } );
            agent( "fractal_agent_route_task", "FRACTAL.AGENT.ROUTE_TASK",
                "Route a task to the nearest capability under a token budget, with a written justification.",
                { Key( "capability_key", "Corpus key, one capability embedding per row." ), Vec( "task", "The task embedding." ),
                  Int( "budget", "Tokens available." ), Int( "cost_per_route", "Tokens spent per routing." ).KW( "COST_PER_ROUTE" ).O( 150 ), Key( "ids", kIds ).KW( "IDS" ) } );
            agent( "fractal_agent_outlier_intercept", "FRACTAL.AGENT.OUTLIER_INTERCEPT",
                "Intercept a proposed state that is too close to a known-bad one. Returns intercepted, the nearest distance and a reason.",
                { Key( "history_key", "Corpus key of known-bad states." ), Vec( "state", "The proposed state." ),
                  Num( "threshold", "Intercept when the nearest distance is below this." ),
                  Str( "metric", "cosine compares shape, l2 compares magnitude too." ).KW( "METRIC" ).O( "cosine" ).OneOf( { "cosine", "l2" } ) } );
            agent( "fractal_agent_recall_hybrid", "FRACTAL.AGENT.RECALL_HYBRID",
                "Pure retrieval, no model: the K nearest memories to a query, optionally restricted to a cohort.",
                { Key( "corpus_key", "Corpus key." ), Vec( "query", "The query vector." ), Int( "k", "How many." ).KW( "K" ).O( 5 ),
                  Key( "ids", kIds ).KW( "IDS" ), Key( "content", kContent ).KW( "CONTENT" ),
                  Filter( "filter", "Cohort restriction: {hash, value} keeps rows whose entry in that hash equals value." ).KW( "FILTER" ) }, false );
            agent( "fractal_agent_recommend_diverse", "FRACTAL.AGENT.RECOMMEND_DIVERSE",
                "Pure retrieval, no model: K recommendations that are relevant and mutually diverse.",
                { Key( "catalog_key", "Catalog corpus key." ), Vec( "query", "The query vector." ), Int( "k", "How many." ).KW( "K" ).O( 10 ), Key( "ids", kIds ).KW( "IDS" ) }, false );
            agent( "fractal_agent_data_analyst", "FRACTAL.AGENT.DATA_ANALYST",
                "Natural-language question over a valkey-search index: the model writes a query, the module validates and runs it under YOUR ACL, and the model analyses the rows. Needs valkey-search and a model.",
                { Str( "index", "The valkey-search index name." ), Str( "question", "The question (1 to 4096 bytes)." ),
                  Int( "max_attempts", "Correction attempts (1 to 5)." ).KW( "MAX_ATTEMPTS" ), Int( "limit", "Most rows returned (1 to 100)." ).KW( "LIMIT" ),
                  JsonText( "context", kCtx ).KW( "CONTEXT" ) } );
            agent( "fractal_agent_patient_deterioration_triage", "FRACTAL.AGENT.PATIENT_DETERIORATION_TRIAGE",
                "Cohort search plus the drift from baseline to current, then a written triage. Returns cohort_matches, a projected trajectory and a summary.",
                { Key( "corpus_key", "The cohort corpus." ), Vec( "query", "The entity's current profile." ), Vec( "baseline", "Intake vector." ),
                  Vec( "current", "Present vector." ), Int( "k", "Cohort size." ).KW( "K" ).O( 5 ), Key( "ids", kIds ).KW( "IDS" ),
                  Filter( "filter", "Cohort restriction: {hash, value}; restricts the cohort search, not the drift search." ).KW( "FILTER" ) } );
            agent( "fractal_agent_feedback_audit", "FRACTAL.AGENT.FEEDBACK_AUDIT",
                "Self-contained diversify health check: warm a session with the first rows of a corpus, search, and report the diversity quotient. No model.",
                { Key( "catalog_key", "Catalog corpus key." ), Vec( "query", "The audited query." ), Key( "warmup_key", "Corpus whose first rows warm the session." ),
                  Int( "warmup_count", "Warmup rows (at most 256)." ).KW( "WARMUP_COUNT" ).O( 8 ), Int( "k", "Results." ).KW( "K" ).O( 3 ) }, false );
            agent( "fractal_agent_schedule_workload", "FRACTAL.AGENT.SCHEDULE_WORKLOAD",
                "Place a workload on the nearest node by capability vector, with a written justification and confidence.",
                { Key( "node_key", "Corpus key of node capability vectors." ), Vec( "task", "The workload requirement vector." ),
                  Int( "iterations", "Search iterations." ).KW( "ITERATIONS" ).O( 30 ), Int( "population", "Search population." ).KW( "POPULATION" ).O( 50 ),
                  JsonText( "context", kCtx ).KW( "CONTEXT" ), Key( "ids", kIds ).KW( "IDS" ) } );
            agent( "fractal_agent_rebalance_sibling", "FRACTAL.AGENT.REBALANCE_SIBLING",
                "Optimise a portfolio, find the most similar past allocation by weight change, and explain the rebalance.",
                { Key( "alloc_key", "Corpus key of past allocations, one weight vector per row." ), Vec( "mu", "Expected returns." ),
                  Mat( "cov", "Covariance matrix." ), Int( "cardinality", "At most this many assets." ), Vec( "baseline", "The baseline weights." ),
                  Int( "seed", "Makes the optimiser deterministic." ).KW( "SEED" ), Key( "ids", kIds ).KW( "IDS" ), JsonText( "context", kCtx ).KW( "CONTEXT" ) } );
            agent( "fractal_agent_detour_classify", "FRACTAL.AGENT.DETOUR_CLASSIFY",
                "Classify a vehicle's detour: nearest fleet peer by route change and the box-counting complexity of its GPS trace, with a written verdict.",
                { Key( "corpus_key", "Fleet deviation vectors, one per row." ), Vec( "baseline", "Baseline route embedding." ), Vec( "current", "Current route embedding." ),
                  Key( "gps_trace_key", "Key of the flattened GPS trace." ), Int( "boxcount_dim", "Values per trace point." ).KW( "BOXCOUNT_DIM" ).O( 2 ), Key( "ids", kIds ).KW( "IDS" ) } );
            agent( "fractal_agent_track_anomaly", "FRACTAL.AGENT.TRACK_ANOMALY",
                "Track deviation check: nearest peer plus the DFA exponent of the heading series, with a written assessment.",
                { Key( "corpus_key", "Fleet deviation signatures." ), Vec( "baseline", "Baseline state." ), Vec( "current", "Current state." ),
                  Key( "heading_series_key", "Key of the heading-change series." ), Key( "ids", kIds ).KW( "IDS" ) } );
            agent( "fractal_agent_network_coverage_alert", "FRACTAL.AGENT.NETWORK_COVERAGE_ALERT",
                "Sensor-grid coverage: morphological dimension and lacunarity of the point cloud plus drift in the telemetry, then an alert.",
                { Key( "point_cloud_key", "Key of the flattened sensor coordinates." ), Key( "drift_series_key", "Key of the telemetry series." ),
                  Int( "boxcount_dim", "Values per point." ).O( 2 ), Int( "drift_win", "Drift window." ).O( 48 ),
                  Num( "drift_threshold", "Drift threshold." ).O( 0.5 ), JsonText( "context", kCtx ).O( "{}" ) } );
            agent( "fractal_agent_regime_triage", "FRACTAL.AGENT.REGIME_TRIAGE",
                "General-purpose regime check on any series: DFA exponent and drift, then a written triage.",
                { Key( "series_key", "Key of the series." ), Int( "win", "Window (at least 16)." ).O( 64 ),
                  Num( "drift_threshold", "Drift threshold." ).O( 0.5 ), JsonText( "context", kCtx ).O( "{}" ) } );
            agent( "fractal_agent_diverse_portfolios", "FRACTAL.AGENT.DIVERSE_PORTFOLIOS",
                "Enterprise: several mutually different near-optimal portfolios (sharpe) or the return-versus-risk front (pareto), with a written comparison.",
                { Vec( "mu", "Expected returns." ), Mat( "cov", "Covariance matrix." ), Int( "cardinality", "At most this many assets." ),
                  Int( "n_restarts", "Independent searches (1 to 64)." ).KW( "N_RESTARTS" ).O( 8 ), Num( "overlap", "How much two portfolios may share (0 to 1)." ).KW( "OVERLAP" ),
                  Num( "quality", "How close to the best Sharpe (above 0, at most 1)." ).KW( "QUALITY" ), Int( "seed", "Repeatability." ).KW( "SEED" ),
                  JsonText( "context", kCtx ).KW( "CONTEXT" ), Str( "objective_mode", "sharpe or pareto." ).KW( "OBJECTIVE_MODE" ).O( "sharpe" ).OneOf( { "sharpe", "pareto" } ) } );
            agent( "fractal_agent_plan_explore", "FRACTAL.AGENT.PLAN_EXPLORE",
                "Embed a seed state and branch it into the nearest strategies. Needs an embedding provider.",
                { Key( "strategy_key", "Corpus key of strategy embeddings." ), Str( "state_text", "The seed state, text." ),
                  Int( "dim", "The corpus dimension (must equal the embedding width)." ), Int( "max_branches", "How many branches (1 to 100000)." ) } );
            agent( "fractal_agent_trajectory_predict", "FRACTAL.AGENT.TRAJECTORY_PREDICT",
                "Project the next state from a time-ordered state log: the drift since a baseline row, applied to the current state. No model.",
                { Key( "corpus_key", "Key of dim-wide state rows in time order; the last row is current." ), Int( "dim", "The row width." ),
                  Int( "baseline_row", "Index of the baseline row." ) }, false );
            agent( "fractal_agent_detect_loop", "FRACTAL.AGENT.DETECT_LOOP",
                "Detect an agent stuck in a loop: fingerprint each logged state and find a cycle, tolerating a few differing bits. No model.",
                { Str( "agent_id", "A label echoed back." ), Key( "state_log_key", "Key of the log, dim floats per state." ), Int( "dim", "State width." ),
                  Int( "n_bits", "Fingerprint bits (1 to 4096)." ).KW( "N_BITS" ).O( 64 ), Int( "seed", "Fingerprint seed." ).KW( "SEED" ).O( 42 ),
                  Int( "hamming_threshold", "Bits two fingerprints may differ and still match." ).KW( "HAMMING_THRESHOLD" ).O( 0 ) }, false );
            agent( "fractal_search_agent", "FRACTAL.SEARCH.AGENT",
                "Embed a question, search the corpus, and answer from the hits. Needs an embedding provider and a model.",
                { Key( "corpus_key", "Corpus key." ), Str( "query_text", "The question." ), Int( "dim", "Corpus dimension (must equal the embedding width)." ),
                  Int( "population", "Search population." ).KW( "POPULATION" ), Int( "iterations", "Search iterations." ).KW( "ITERATIONS" ),
                  Key( "content", kContent ).KW( "CONTENT" ), Key( "ids", kIds ).KW( "IDS" ) } );
            agent( "fractal_rag_agent", "FRACTAL.RAG.AGENT",
                "Retrieval-augmented answer: search_agent with a fixed search. Returns the answer, source_doc_ids and timing.",
                { Key( "corpus_key", "Corpus key." ), Str( "query_text", "The question." ), Int( "dim", "Corpus dimension." ),
                  Key( "content", kContent ).KW( "CONTENT" ), Key( "ids", kIds ).KW( "IDS" ) } );

            // ---- analytics ----
            auto an = [ & ]( const char* name, const char* cmd, const char* desc, std::vector<Prm> ps, Out out = Out::Auto, ToolClass cls = ToolClass::Cheap )
            { add( tool( name, "analytics", cmd, cls, desc, std::move( ps ), true, false, false, out ) ); };
            an( "fractal_dimension_dfa", "FRACTAL.DIM.DFA", "Detrended fluctuation analysis exponent of a series: about 0.5 for noise, above 1 for persistent trends.",
                { Vec( "series", "The series, at least a few dozen samples." ) }, Out::Number );
            an( "fractal_dimension_drift", "FRACTAL.DIM.DRIFT", "Regime drift: the DFA exponent of the recent window against the baseline before it.",
                { Vec( "series", "The series." ), Int( "win", "The recent window." ).O( 48 ) } );
            an( "fractal_dimension_boxcount", "FRACTAL.DIM.BOXCOUNT", "Box-counting fractal dimension of a point cloud.",
                { Mat( "points", "Points as equal-length rows." ), DimOf( "points" ) } );
            an( "fractal_periodogram", "FRACTAL.SERIES.PERIODOGRAM", "Dominant frequencies of a series: the strongest spectral peaks.",
                { Vec( "series", "The series." ), Int( "max_peaks", "Most peaks to return." ).O( 8 ) } );
            an( "fractal_change_point_detect", "FRACTAL.SERIES.CHANGEPOINT", "Indices where the series' behaviour changes.",
                { Vec( "series", "The series." ), Int( "window", "Comparison window." ), Num( "threshold", "Sensitivity." ).O( 2.0 ), Int( "max_points", "Most indices to return." ).O( 16 ) } );
            an( "fractal_state_fingerprint", "FRACTAL.SERIES.FINGERPRINT", "A compact bit fingerprint of a vector (random-hyperplane hash); returns hex bytes.",
                { Vec( "vector", "The vector." ), Int( "n_bits", "Fingerprint bits." ).O( 128 ), Int( "seed", "Seed." ).O( 0 ) }, Out::Hex );
            an( "fractal_spectrogram_entropy", "FRACTAL.SERIES.SPECTROGRAM", "Spectral entropy per sliding frame: low for tonal signals, near 1 for noise.",
                { Vec( "series", "The series." ), Int( "window", "Frame size (8 to 512)." ).O( 64 ), Int( "hop", "Step between frames." ).O( 32 ), Int( "max_frames", "Most frames." ).O( 4096 ) } );
            an( "fractal_cycle_detect", "FRACTAL.CYCLE.DETECT", "Find a cycle in a stream of fingerprints (Brent's algorithm), tolerating a few differing bits.",
                { HexList( "fingerprints", "Fingerprints as equal-length hex strings, in time order." ), BytesOf( "fingerprints" ), Int( "hamming_threshold", "Bits that may differ." ).O( 0 ) } );
            an( "fractal_tda_persistence_diagram", "FRACTAL.TDA.PERSISTENCE", "Persistent homology of a point cloud: H0 and H1 barcodes.",
                { Mat( "points", "Points as equal-length rows." ), DimOf( "points" ), Int( "max_dim", "0 or 1." ).O( 0 ), Num( "max_thresh", "Largest filtration distance." ), Int( "max_bars", "Most bars." ).O( 64 ) },
                Out::Auto, ToolClass::Expensive );
            an( "fractal_morphological_complexity", "FRACTAL.MORPH.COMPLEXITY", "Fractal dimension and lacunarity of a point cloud.",
                { Mat( "points", "Points as equal-length rows." ), DimOf( "points" ) } );
            an( "fractal_cortical_folding", "FRACTAL.MORPH.CORTICAL", "Gyrification index of a mesh: surface area against its convex hull.",
                { Mat( "vertices", "x,y,z rows." ).W( 3 ), UInts( "faces", "Vertex-index triples, flat." ) } );
            an( "fractal_nerve_plexus_metric", "FRACTAL.MORPH.NERVE", "Fibre-length density and branch density of a nerve skeleton.",
                { Mat( "coords", "Node coordinates as equal-length rows." ), DimOf( "coords" ), UInts( "edges", "Node-index pairs, flat." ) } );
            an( "fractal_vascular_network", "FRACTAL.MORPH.VASCULAR", "Tortuosity, branch density and fractal dimension of a vessel tree.",
                { Mat( "coords", "x,y,z rows." ).W( 3 ), UInts( "edges", "Node-index pairs, flat." ), Vec( "arc_lengths", "One arc length per edge." ) } );
            an( "fractal_optimize_portfolio", "FRACTAL.OPTIMIZE.PORTFOLIO", "Cardinality-constrained Sharpe maximisation by Stochastic Fractal Search.",
                { Vec( "mu", "Expected returns." ), Mat( "cov", "The n x n covariance matrix." ), Int( "k", "At most this many assets." ),
                  Int( "seed", "Repeatability." ).O(), Bool( "use_obl", "Opposition-based learning per trial (off by default)." ).O( false ),
                  Str( "diffusion_mode", "gaussian or levy." ).O( "gaussian" ).OneOf( { "gaussian", "levy" } ) }, Out::Auto, ToolClass::Expensive );
            // vector math
            auto vm = [ & ]( const char* name, const char* cmd, const char* desc, std::vector<Prm> ps, Out out )
            { add( tool( name, "analytics", cmd, C, desc, std::move( ps ), true, false, false, out ) ); };
            vm( "fractal_vector_lp_distance", "FRACTAL.VECTOR.LP", "Minkowski L_p distance.", { Vec( "a", "First vector." ), Vec( "b", "Second vector." ), Num( "p", "The exponent, above 0." ) }, Out::Number );
            vm( "fractal_vector_hamming_distance", "FRACTAL.VECTOR.HAMMING", "Differing bits between two equal-length binary strings (hex).", { Hex( "a", "First, hex." ), Hex( "b", "Second, hex." ) }, Out::Number );
            vm( "fractal_vector_quantize_int8", "FRACTAL.VECTOR.QUANTIZE.INT8", "Scalar int8 quantisation: returns scale and codes (4x smaller).", { Vec( "vector", "The vector." ) }, Out::Auto );
            vm( "fractal_vector_quantize_binary", "FRACTAL.VECTOR.QUANTIZE.BINARY", "Sign-bit quantisation: one bit per dimension, as hex bytes.", { Vec( "vector", "The vector." ) }, Out::Hex );

            // ---- enterprise ----
            auto ent = [ & ]( const char* name, const char* cmd, const char* desc, std::vector<Prm> ps, bool ro, bool destructive, Out out = Out::Auto, ToolClass cls = ToolClass::Cheap )
            { add( tool( name, "enterprise", cmd, cls, desc, std::move( ps ), ro, destructive, false, out ) ); };
            ent( "fractal_ledger_verify", "FRACTAL.LEDGER.VERIFY", "Walk a ledger chain and report whether it is intact (ok, rows_verified, or the first failure). Needs the daemon.",
                 { Int( "kind", "1 = QTL ledger, 2 = decision-audit chain." ).O( 1 ) }, true, false );
            ent( "fractal_ledger_truth_count", "FRACTAL.LEDGER.TRUTH.COUNT", "Entries in the truth ledger. Needs the enterprise tier.", {}, true, false );
            ent( "fractal_ledger_shadow_count", "FRACTAL.LEDGER.SHADOW.COUNT", "Entries in the shadow ledger. Needs the enterprise tier.", {}, true, false );
            ent( "fractal_ledger_flush", "FRACTAL.LEDGER.FLUSH", "Persist the ledger to its file. Needs the enterprise tier.", {}, false, true );
            ent( "fractal_ledger_load", "FRACTAL.LEDGER.LOAD", "Load the persisted ledger into memory. Needs the enterprise tier.", {}, false, true );
            ent( "fractal_ledger_compact", "FRACTAL.LEDGER.COMPACT", "Compact the ledger. Needs the enterprise tier.", {}, false, true );
            ent( "fractal_ledger_reset_soft", "FRACTAL.LEDGER.RESET.SOFT", "Clear the in-memory shadow state, keeping the truth ledger. Needs the enterprise tier.", {}, false, true );
            ent( "fractal_ledger_reset_hard", "FRACTAL.LEDGER.RESET.HARD", "Erase the ledger entirely. Irreversible. Needs the enterprise tier.", {}, false, true );
            ent( "fractal_audit_log", "FRACTAL.AUDIT.LOG", "Append a record to the tamper-evident decision-audit chain (kind 2). Needs the enterprise tier.",
                 { Str( "entry_type", "A short label for the record." ), JsonText( "payload", "The record, a JSON object." ) }, false, false );
            ent( "fractal_audit_unpack", "FRACTAL.AUDIT.UNPACK", "Decode a persisted audit blob (hex) into its JSON.", { Hex( "blob", "The blob, hex." ) }, true, false );
            ent( "fractal_optimize_portfolio_multimodal", "FRACTAL.OPTIMIZE.MULTIMODAL", "Enterprise: several mutually different near-optimal portfolios.",
                 { Vec( "mu", "Expected returns." ), Mat( "cov", "Covariance matrix." ), Int( "k", "At most this many assets." ), Int( "n_restarts", "Independent searches (1 to 64)." ).O( 8 ),
                   Num( "overlap_threshold", "How much two portfolios may share (0 to 1)." ).O( 0.15 ), Num( "quality_frac", "How close to the best Sharpe (above 0, at most 1)." ).O( 0.9 ), Int( "seed", "Repeatability." ).O() },
                 true, false, Out::Auto, ToolClass::Expensive );
            ent( "fractal_optimize_portfolio_multimodal_pareto", "FRACTAL.OPTIMIZE.MULTIMODAL.PARETO", "Enterprise: the non-dominated return-versus-risk front.",
                 { Vec( "mu", "Expected returns." ), Mat( "cov", "Covariance matrix." ), Int( "k", "At most this many assets." ), Int( "n_restarts", "Independent searches." ).O( 8 ),
                   Int( "max_front", "Largest front returned." ).O( 8 ), Int( "seed", "Repeatability." ).O() },
                 true, false, Out::Auto, ToolClass::Expensive );

            // ---- keyspace helpers (the only tools that write non-FractalSQL keys) ----
            { Tool x = tool( "fractal_store_vectors", "keyspace", "SET", C,
                "Store rows as a corpus: packs the rows as float32 into one key so fractal_search and the agents can use it. Honours FSQL_MCP_KEY_PREFIX.",
                { Key( "key", "The corpus key to write." ), Mat( "rows", "Equal-length rows." ), Bool( "overwrite", "Replace an existing key (default true)." ).O( true ) }, false, true );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& c ) { return storeVectors( db, t, a, c.clientKey ); }; add( std::move( x ) ); }
            { Tool x = tool( "fractal_store_series", "keyspace", "SET", C,
                "Store a numeric series as packed float32 under a key, for the series tools and agents. Honours FSQL_MCP_KEY_PREFIX.",
                { Key( "key", "The key to write." ), Vec( "values", "The samples." ), Bool( "overwrite", "Replace an existing key (default true)." ).O( true ) }, false, true );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& c ) { return storeSeries( db, t, a, c.clientKey ); }; add( std::move( x ) ); }
            { Tool x = tool( "fractal_store_hash", "keyspace", "HSET", C,
                "Write a hash: the IDS / CONTENT / FILTER companions the agents read (field = row index). Honours FSQL_MCP_KEY_PREFIX.",
                { Key( "key", "The hash key." ), Obj( "fields", "Field -> string or number." ) }, false, true );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& c ) { return storeHash( db, t, a, c.clientKey ); }; add( std::move( x ) ); }
            { Tool x = tool( "fractal_delete_key", "keyspace", "DEL", C,
                "Delete a key (a corpus, series or hash this server wrote). Honours FSQL_MCP_KEY_PREFIX.", { Key( "key", "The key." ) }, false, true );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& c ) { return deleteKey( db, t, a, c.clientKey ); }; add( std::move( x ) ); }
            { Tool x = tool( "fractal_key_info", "keyspace", "TYPE", C,
                "Describe a key: its type and, for a corpus or series, how many float32 values it holds.", { Key( "key", "The key." ) } );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& ) { return keyInfo( db, t, a ); }; add( std::move( x ) ); }

            // ---- admin ----
            { Tool x = tool( "fractal_version", "admin", "FRACTAL.VERSION", C,
                "The module's version and edition, the Valkey server version and this MCP server's version.", {} );
              x.custom = []( Db& db, const Tool& t, const Json& a, CallCtx& ) { return versionTool( db, t, a ); }; add( std::move( x ) ); }

            // Apply the per-tool annotation overrides to the finished table.
            for( Tool& t : v )
                for( const Annotation& a : kAnnotations )
                    if( t.name == a.name )
                    {
                        t.readOnly = a.readOnly;
                        t.destructive = a.destructive;
                        t.secret = a.secret;
                    }
            return v;
        }();
        return t;
    }
}   // namespace

std::vector<ToolSpec> buildCommandTools( Db& db, const RowLimitConfig& )
{
    g_keyPrefix = envOr( "FSQL_MCP_KEY_PREFIX", "" );
    if( g_keyPrefix.empty() )
    {
        // Every client shares one Valkey ACL user, so with no prefix the store_* / delete_key tools can
        // overwrite or delete ANY key, including another client's. Warn always; refuse to start when the
        // operator has declared a prefix mandatory.
        logging::logInfo( "WARNING: FSQL_MCP_KEY_PREFIX is empty; the store/delete tools can touch any key in the keyspace. "
                          "Set a prefix (e.g. 'mcp:') to confine them." );
        if( envBoolOr( "FSQL_MCP_REQUIRE_KEY_PREFIX", false ) )
            throw ToolError( ErrorClass::Schema, "FSQL_MCP_REQUIRE_KEY_PREFIX is set but FSQL_MCP_KEY_PREFIX is empty" );
    }
    std::vector<ToolSpec> out;
    for( const Tool& t : table() )
    {
        ToolSpec s;
        s.name = t.name; s.category = t.category; s.toolClass = t.cls;
        s.description = t.desc;
        s.readOnlyHint = t.readOnly; s.destructiveHint = t.destructive; s.openWorldHint = t.openWorld; s.secretHint = t.secret;
        for( const Prm& p : t.params )
        {
            if( p.kind == K::DimOf || p.kind == K::BytesOf ) continue;
            ToolParam tp{ p.name, jsonTypeOf( p.kind ), p.req, p.def, p.desc, false, itemTypeOf( p.kind ) };
            s.params.push_back( tp );
        }
        const Tool* tp = &t;
        Db* dbp = &db;
        if( t.custom )
            s.handler = [ dbp, tp ]( const Json& a, CallCtx& c ) { return tp->custom( *dbp, *tp, a, c ); };
        else
            s.handler = [ dbp, tp ]( const Json& a, CallCtx& c )
            {
                // The module records the Valkey client id and ACL user (the MCP server's own);
                // this adds the MCP client key so the entry also names which MCP client asked.
                if( tp->name == "fractal_audit_log" && a.contains( "payload" ) && a.at( "payload" ).is_object() )
                {
                    Json b = a;
                    b[ "payload" ][ "mcp_client" ] = c.clientKey;
                    return runTable( *dbp, *tp, b, c );
                }
                return runTable( *dbp, *tp, a, c );
            };
        out.push_back( std::move( s ) );
    }
    return out;
}

}   // namespace fsqlmcp
