#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# Pre-remove hook for the Alpine .apk: stops the service and removes it from the runlevel. The key, the config
# and the fractalsql user are kept, so a reinstall does not regenerate the shared key.
set -e

rc-service fractalsqld stop >/dev/null 2>&1 || true
rc-update del fractalsqld default >/dev/null 2>&1 || true
