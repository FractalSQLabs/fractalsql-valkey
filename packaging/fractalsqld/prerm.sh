#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Daniel Gardiner d/b/a FractalSQLabs
#
# --before-remove hook shared by the .deb and the .rpm. Stops and disables
# fractalsqld. Runs on an upgrade's remove-old step and on a true removal; both
# are safe because a new version's postinst enables and restarts it again. The
# key, the config and the fractalsql user are never deleted here, so a reinstall
# does not need to regenerate the shared HMAC key.
set -e

systemctl stop fractalsqld.service >/dev/null 2>&1 || true
systemctl disable fractalsqld.service >/dev/null 2>&1 || true
