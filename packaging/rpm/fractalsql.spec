%global moddir   /usr/lib/valkey/modules
%global confdir  /etc/valkey/modules-available

Name:           fractalsql-valkey
Version:        2.0.0
Release:        1%{?dist}
Summary:        Stochastic Fractal Search module for Valkey

License:        Apache-2.0 AND BSD-2-Clause
URL:            https://github.com/FractalSQLabs/fractalsql-valkey
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc, make
Requires:       valkey
Recommends:     valkey-tools
Suggests:       fractalsqld-valkey

BuildArch:      %{_arch}

%description
fractalsql-valkey is a native C module for Valkey 7.2 / 8.0 / 8.1 that
adds the FractalSQL surface as ordinary commands: FRACTAL.SEARCH and
FRACTAL.EXPLORE (Stochastic Fractal Search retrieval, Sniper and Scout),
named diversify/repulsion sessions, FRACTAL.VECTOR.* math and quantization,
dimension, series, topology, geometry and portfolio analytics, and twenty-one
FRACTAL.AGENT.* commands that decide from data you already keep in keys.
Reasoning, embeddings and the optional enterprise ledger go through the
separate fractalsqld daemon, which the module reaches over a local socket;
everything else runs inside the server on worker threads, so a long search
never blocks other clients. The FractalSQL core is statically linked, and
the only runtime dependency is glibc.

Built against Valkey's self-contained redismodule.h. The module ABI
(REDISMODULE_APIVER_1, aliased as VALKEYMODULE_APIVER_1) was preserved
across the April 2024 fork from Redis, so one .so per arch covers every
currently-shipping Valkey major.

After installation, enable the module by adding the following line
to valkey.conf (the post-install script prints this):

    loadmodule /usr/lib/valkey/modules/fractalsql.so

An example config snippet is installed at
/etc/valkey/modules-available/fractalsql.conf.

%prep
%setup -q

%build
# The per-arch .so is produced out-of-band by build.sh on a Docker
# builder; this spec stages the prebuilt artifact.
test -f dist/%{_arch}/fractalsql.so

%install
# Claim %dir ownership of the Valkey modules directory since no base
# package owns it on stock RHEL-family installs.
install -d -m 0755 %{buildroot}%{moddir}
install -d -m 0755 %{buildroot}%{confdir}

install -Dm0755 dist/%{_arch}/fractalsql.so \
    %{buildroot}%{moddir}/fractalsql.so
install -Dm0644 scripts/load_module.conf \
    %{buildroot}%{confdir}/fractalsql.conf
install -Dm0644 LICENSE \
    %{buildroot}%{_docdir}/%{name}/LICENSE
install -Dm0644 THIRD-PARTY-NOTICES.md \
    %{buildroot}%{_docdir}/%{name}/LICENSE-THIRD-PARTY

%files
%license LICENSE
%license THIRD-PARTY-NOTICES.md
%dir %{moddir}
%dir %{confdir}
%{moddir}/fractalsql.so
%{_docdir}/%{name}/LICENSE
%{_docdir}/%{name}/LICENSE-THIRD-PARTY
%config(noreplace) %{confdir}/fractalsql.conf

%post
cat <<'EOF'

fractalsql-valkey installed.

The module is at:
    /usr/lib/valkey/modules/fractalsql.so

Enable it by adding this to valkey.conf:

    loadmodule /usr/lib/valkey/modules/fractalsql.so

Or include the shipped snippet:

    include /etc/valkey/modules-available/fractalsql.conf

Then restart the server:

    sudo systemctl restart valkey

Verify:

    valkey-cli MODULE LIST
    valkey-cli FRACTAL.EDITION       # -> "Community"
    valkey-cli FRACTAL.VERSION       # -> the installed release
    valkey-cli COMMAND INFO FRACTAL.SEARCH

EOF

%changelog
* Sat Nov 14 2026 FractalSQLabs <ops@fractalsqlabs.io> - 2.0.0-1
- 2.0.0 release.
* Fri Oct 30 2026 FractalSQLabs <ops@fractalsqlabs.io> - 1.0.1-1
- FRACTAL.SEARCH and FRACTAL.EXPLORE now refuse a query or corpus that
  contains a NaN or Inf (previously accepted and passed to the optimizer).
- A NULL or failed result is reported as an error; core error text is
  sanitized before it is placed in a RESP error line.
- The two commands share one input loader (they had drifted).
- The module no longer exports the statically linked core's symbols.
- Re-vendored fractalsql-core (196be156).
* Sat Apr 18 2026 FractalSQLabs <ops@fractalsqlabs.io> - 1.0.0-1
- Initial Factory-standardized release for Valkey 7.2 / 8.0 / 8.1.
  Static LuaJIT, zero-dependency posture. One .so per arch covers
  every currently-shipping Valkey major. Verified on AMD64 and ARM64.
