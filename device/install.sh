#!/usr/bin/env bash
# Install (or remove) the HTTP print API on the Anycubic M5s network board.
#
#   ./device/install.sh [host]              install, default host: anycubic (ssh alias)
#   ./device/install.sh --uninstall [host]  restore stock uhttpd config and remove files
#
# Files are streamed over ssh with tar (dropbear has no sftp-server, so modern
# `scp` would fail without -O).
set -euo pipefail

UNINSTALL=0
if [[ "${1:-}" == "--uninstall" ]]; then UNINSTALL=1; shift; fi
HOST="${1:-anycubic}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if (( UNINSTALL )); then
	echo ">> Uninstalling print API from $HOST"
	ssh -T "$HOST" 'sh -s' <<'EOF'
set -e
rm -rf /www/cgi-bin/api /usr/lib/lua/printapi.lua /tmp/printapi.lock /tmp/printapi_files.json
if [ -f /etc/config/uhttpd.orig ]; then
	cp /etc/config/uhttpd.orig /etc/config/uhttpd
	rm /etc/config/uhttpd.orig
	echo "restored stock uhttpd config"
fi
/etc/init.d/uhttpd restart
EOF
	echo ">> Done"
	exit 0
fi

echo ">> Staging files"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
install -D -m 0644 "$HERE/printapi.lua" "$STAGE/usr/lib/lua/printapi.lua"
for f in "$HERE"/www/cgi-bin/api/*; do
	install -D -m 0755 "$f" "$STAGE/www/cgi-bin/api/$(basename "$f")"
done

echo ">> Copying to $HOST"
tar -C "$STAGE" -cf - usr www | ssh -T "$HOST" 'tar -xf - -C /'

echo ">> Configuring uhttpd"
ssh -T "$HOST" 'sh -s' <<'EOF'
set -e
chmod 0755 /www/cgi-bin/api/*
[ -f /etc/config/uhttpd.orig ] || cp /etc/config/uhttpd /etc/config/uhttpd.orig
uci set uhttpd.main.script_timeout=900
uci set uhttpd.main.redirect_https=0
uci commit uhttpd
/etc/init.d/uhttpd restart
uci show uhttpd.main | grep -E 'script_timeout|redirect_https'
EOF

ADDR="$(ssh -G "$HOST" | awk '$1 == "hostname" { print $2 }')"
echo ">> Smoke test: http://$ADDR/cgi-bin/api/status"
sleep 1
curl -fsS "http://$ADDR/cgi-bin/api/status"
echo
echo ">> Installed"
