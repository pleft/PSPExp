#!/bin/sh
# The package manager's failure paths (docs/Package_Manager.md §10.3), on a
# Debian board set up by pspexp-apt-setup. Run as the console's user, with
# sudo (setup and restore only), by tests/pkg/run.py, which puts the signed
# test catalogues in /tmp/pkgtest-repo/{good,tampered,older}/.
# Uses throwaway recipes: pkgtest-hello (Debian's tiny "hello"), pkgtest-sys
# (a system on the NES core), pkgtest-clash (a system named like NES's).
ROOT=${PSPEXP_ROOT:-/opt/pspexp}; APPS=$ROOT/apps; SYS=$ROOT/systems; ST=$ROOT/state
CAT=/usr/share/pspexp/catalogue
for HELPER in /usr/sbin/pspexp-pkg-apt /usr/local/sbin/pspexp-pkg-apt; do [ -x "$HELPER" ] && break; done   # pspexp-core's, or an installer's
pass=0; fail=0
ok() { pass=$((pass + 1)); echo "  PASS $*"; }
bad() { fail=$((fail + 1)); echo "  FAIL $*"; }
check() { if eval "$1"; then ok "$2"; else bad "$2 ($1)"; fi; }
meta() { dpkg-query -W -f='${Status} ${Version}' "pspexp-recipe-$1" 2>/dev/null | awk '/install ok installed/ {print $NF}'; }
has() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }
clean() { [ ! -f "$ST/txn" ] && [ ! -f "$ST/publishing" ] && [ ! -d "$ST/stage/$1" ] && [ ! -d "$APPS/.$1.old" ]; }
installed_ok() {   # the recipe fully installed: meta, launcher files, nothing left over
	[ -n "$(meta "$1")" ] && [ -f "$APPS/$1/package" ] && grep -q "^recipe=$1" "$APPS/$1/package" && clean "$1" &&
	{ [ "$1" != pkgtest-sys ] || grep -q '^pkg=pkgtest-sys' "$SYS/pkgtest-sys.system"; }
}
gone_ok() { [ -z "$(meta "$1")" ] && [ ! -d "$APPS/$1" ] && [ ! -f "$SYS/$1.system" ] && clean "$1"; }
crashrun() { PSPEXP_PKG_CRASH="$1" pspexp-pkg $2 "$3" >/dev/null 2>&1; }

REPO_SAVE=$(cat /etc/pspexp/repo)
sudo cp -a "$CAT" /tmp/pkgtest-catalogue-save
setrepo() { echo "$1" | sudo tee /etc/pspexp/repo >/dev/null; }
restore() {
	setrepo "$REPO_SAVE"
	sudo rm -rf "$CAT" && sudo cp -a /tmp/pkgtest-catalogue-save "$CAT" && sudo rm -rf /tmp/pkgtest-catalogue-save
}
trap restore EXIT

echo "catalogue: authentication (§5.1)"
setrepo file:///tmp/pkgtest-repo/good; sudo $HELPER update >/dev/null 2>&1
check '[ -d $CAT/recipes/pkgtest-hello ]' "a signed, newer catalogue is taken"
S1=$(cat $CAT/serial)
setrepo file:///tmp/pkgtest-repo/tampered
sudo $HELPER update >/dev/null 2>&1; r=$?
check '[ $r -ne 0 ] && [ "$(cat $CAT/serial)" = "$S1" ]' "a tampered catalogue is refused; the installed one stays"
setrepo file:///tmp/pkgtest-repo/older
sudo $HELPER update >/dev/null 2>&1; r=$?
check '[ $r -ne 0 ] && [ "$(cat $CAT/serial)" = "$S1" ]' "an older (validly signed) catalogue is refused"
setrepo "$REPO_SAVE"

echo "install: interrupted at each checkpoint and between action and record (§5.3)"
for p in staged apt-started apt-unrecorded apt-done app-swap-started app-swap-half app-swap-unrecorded app-swapped systems-started system-renamed; do
	id=pkgtest-hello; case "$p" in systems-started|system-renamed) id=pkgtest-sys ;; esac
	crashrun "$p" install $id
	pspexp-pkg recover >/dev/null 2>&1
	if [ "$p" = staged ]; then check "gone_ok $id" "killed at $p: recover leaves nothing changed"
	else check "installed_ok $id" "killed at $p: recover finishes the install"; fi
	pspexp-pkg remove $id >/dev/null 2>&1
done

echo "remove: interrupted (§5.3)"
for p in staged app-swap-started systems-started apt-started apt-unrecorded; do
	id=pkgtest-hello; [ "$p" = systems-started ] && id=pkgtest-sys
	pspexp-pkg install $id >/dev/null 2>&1
	crashrun "$p" remove $id
	pspexp-pkg recover >/dev/null 2>&1
	if [ "$p" = staged ]; then check "installed_ok $id" "remove killed at $p: nothing changed"
	else check "gone_ok $id" "remove killed at $p: recover finishes removing"; fi
	pspexp-pkg remove $id >/dev/null 2>&1
done

echo "dpkg interrupted mid-install (§5.3)"
crashrun apt-started install pkgtest-hello
(cd /tmp && apt-get download hello >/dev/null 2>&1 && sudo dpkg --unpack /tmp/hello_*.deb >/dev/null 2>&1)
check 'dpkg-query -W -f="\${Status}" hello | grep -q unpacked' "hello left unpacked, not configured"
pspexp-pkg recover >/dev/null 2>&1
check "installed_ok pkgtest-hello" "recover repairs dpkg and finishes the install"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1; rm -f /tmp/hello_*.deb

echo "recover works from the operation's own record, not the catalogue (§5.3)"
crashrun apt-started install pkgtest-hello
sudo mv "$CAT/recipes/pkgtest-hello" /tmp/pkgtest-hello-recipe   # the recipe gone from the catalogue meanwhile
pspexp-pkg recover >/dev/null 2>&1
check "installed_ok pkgtest-hello" "killed at apt-started, the recipe then gone: recover still finishes the install"
sudo mv /tmp/pkgtest-hello-recipe "$CAT/recipes/pkgtest-hello"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1
crashrun apt-done install pkgtest-hello
f=$(find "$ST/stage/pkgtest-hello/app" -type f ! -name MANIFEST | head -1); echo damaged >> "$f"
pspexp-pkg recover >/dev/null 2>&1
check '[ ! -d "$APPS/pkgtest-hello" ] && clean pkgtest-hello' "killed at apt-done, a staged file then damaged: recover publishes nothing of it"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1

echo "damage found after the app folder is swapped in (§5.3)"
crashrun app-swapped install pkgtest-sys
echo damaged >> "$ST/stage/pkgtest-sys/systems/pkgtest-sys.system"
pspexp-pkg recover >/dev/null 2>&1
check '[ ! -d "$APPS/pkgtest-sys" ] && [ ! -f "$SYS/pkgtest-sys.system" ] && clean pkgtest-sys' "killed at app-swapped, a staged system file then damaged: recover publishes nothing of it"
pspexp-pkg remove pkgtest-sys >/dev/null 2>&1
crashrun app-swapped install pkgtest-hello
echo damaged >> "$APPS/pkgtest-hello/package"
pspexp-pkg recover >/dev/null 2>&1
check '[ ! -d "$APPS/pkgtest-hello" ] && clean pkgtest-hello' "killed at app-swapped, the swapped-in app then damaged: recover takes it out"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1

echo "the helper's record lost: broken, not cancelled (§5.3)"
crashrun apt-started install pkgtest-hello
sudo rm -f /var/lib/pspexp/meta/pspexp-recipe-pkgtest-hello_*.deb
pspexp-pkg recover >/dev/null 2>&1; r=$?
check '[ $r -ne 0 ] && [ -f "$ST/txn" ] && [ -d "$ST/stage/pkgtest-hello" ]' "recover reports it broken and keeps the operation open"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1; r=$?
check '[ $r -eq 0 ] && gone_ok pkgtest-hello' "remove abandons the operation and leaves nothing of it"

echo "one operation at a time (§5.3)"
( exec 9>"$ST/lock"; flock 9; sleep 6 ) &
sleep 1
out=$(PSPEXP_PKG_WAIT=1 pspexp-pkg install pkgtest-hello 2>&1); r=$?
check '[ $r -ne 0 ] && echo "$out" | grep -q busy' "a second operation reports busy"
wait

echo "ownership through apt (§5.4)"
sudo apt-get -y -q install hello >/dev/null 2>&1
pspexp-pkg install pkgtest-hello >/dev/null 2>&1; pspexp-pkg remove pkgtest-hello >/dev/null 2>&1
check "has hello" "a package installed by hand stays when the recipe goes"
sudo apt-get -y -q remove hello >/dev/null 2>&1
pspexp-pkg install nes >/dev/null 2>&1; pspexp-pkg install pkgtest-sys >/dev/null 2>&1; pspexp-pkg remove pkgtest-sys >/dev/null 2>&1
check "has libretro-nestopia && [ -f $SYS/nes.system ]" "a package another recipe needs stays"

echo "a recipe that changes: upgrade (§5.6)"
pspexp-pkg install pkgtest-hello >/dev/null 2>&1
setrepo file:///tmp/pkgtest-repo/good2; pspexp-pkg update >/dev/null 2>&1; setrepo "$REPO_SAVE"
check '[ "$(pspexp-pkg available | grep "^pkgtest-hello|" | cut -d"|" -f3)" = update ]' "the changed recipe shows as an update"
pspexp-pkg upgrade >/dev/null 2>&1
check '[ "$(meta pkgtest-hello)" = 2 ] && has figlet && grep -q figlet $APPS/pkgtest-hello/*.entry && clean pkgtest-hello' "upgrade: the new meta-package, B (figlet) installed, the new launcher files"
check '! has hello' "upgrade: A (hello), needed by nothing now, removed"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1

echo "damaged staging: back to exactly the old install (§5.3)"
setrepo file:///tmp/pkgtest-repo/good; sudo rm -f $CAT/serial; sudo $HELPER update >/dev/null 2>&1   # (the test resets the serial to go back to version 1)
pspexp-pkg install pkgtest-hello >/dev/null 2>&1
before=$(cat $APPS/pkgtest-hello/*.entry)
setrepo file:///tmp/pkgtest-repo/good2; pspexp-pkg update >/dev/null 2>&1; setrepo "$REPO_SAVE"
crashrun app-swap-started install pkgtest-hello
echo corrupted >> "$ST/stage/pkgtest-hello/app/package"
pspexp-pkg recover >/dev/null 2>&1
check '[ "$(cat $APPS/pkgtest-hello/*.entry)" = "$before" ] && clean pkgtest-hello' "damaged staging: the previous launcher files are back, nothing left over"
pspexp-pkg remove pkgtest-hello >/dev/null 2>&1

echo "system files (§5.5)"
out=$(pspexp-pkg install pkgtest-clash 2>&1); r=$?
check '[ $r -ne 0 ] && echo "$out" | grep -q "belongs to nes" && grep -q "^pkg=nes" $SYS/nes.system && [ -z "$(meta pkgtest-clash)" ]' "a system named like another package's is refused, before apt"
pspexp-pkg recover >/dev/null 2>&1

echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
