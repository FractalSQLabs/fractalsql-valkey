<p align="center">
  <img src="../FractalSQLforValkey.jpg" alt="FractalSQL for Valkey" width="720">
</p>

# service/

`fractalsqld`, the daemon that runs `FRACTAL.REASON`, `FRACTAL.EMBED` and the enterprise ledger for the
module. This directory holds the daemon source and the adapter sources it compiles (`adapter/src`,
`adapter/sql`), so the repository builds the daemon on its own.
Valkey calls 16 of its 74 functions; the rest are unused here.

    make -C service daemon        # needs build-essential, libssl-dev, python3
