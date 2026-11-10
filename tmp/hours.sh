#!/usr/bin/env bash
# hours.sh - arvioi työtunnit per päivä koneen aktiivisuusjäljistä.
#
# Lähteet (kaikki valinnaisia; jos lähde puuttuu tai epäonnistuu, se ohitetaan):
#   login     last/wtmp: kirjautumiset ja uloskirjautumiset
#   journal   journalctl: boot, suspend/resume, lukitus/avaus, logind-sessiot
#   jenkins   Jenkins REST API: buildien aloitus- ja loppuajat
#   sliqtly   Sliqtly-palvelimen SQLite-kanta ja repo.git-historia (ks. src_sliqtly)
#   git       commitit (author-suodatus) ja reflog paikallisista repoista
#   shell     bash/zsh-historia (vain jos aikaleimat tallennettu)
#   files     tiedostojen muokkausajat työhakemistoissa
#
# Laskenta: kaikki tapahtumat järjestetään aikajärjestykseen. Tapahtumat, joiden
# väli on <= GAP_MIN minuuttia, kuuluvat samaan työjaksoon. Jakson kesto + PAD_MIN
# (oletettu työ ennen ensimmäistä / jälkeen viimeisen tapahtuman) lasketaan
# aktiiviajaksi. Jakson yksittäinen tapahtuma = PAD_MIN.
# "Span" = päivän ensimmäinen ja viimeinen tapahtuma (yläraja).
#
# Käyttö:
#   ./hours.sh                       # 2026-11-01..2026-11-10
#   ./hours.sh --from 2026-11-01 --to 2026-11-10 --csv out.csv -v
#   ./hours.sh --no-timeline          # ilman aikajanaa (tapahtumat minuutin tarkkuudella)
#   SOURCES="git jenkins" ./hours.sh
#
# Asetukset ympäristömuuttujilla (oletukset alla):
#   FROM, TO                 päivämäärävälin päät (YYYY-MM-DD, TO mukaan lukien)
#   SOURCES                  "login journal jenkins sliqtly git shell files"
#   GAP_MIN=30  PAD_MIN=10   jaksotus
#   NOTES_MAX=25             "Tehty"-listan rivejä per päivä (0 = ei listaa)
#   MIN_EVENTS_DAY=1         päivä näytetään vain jos tapahtumia vähintään näin monta
#   USER_NAME=$USER          login/journal-suodatus
#
#   JENKINS_URL              esim. http://localhost:8081 (tyhjä = ohitetaan)
#   JENKINS_USER, JENKINS_TOKEN   API-token (Jenkins > User > Configure)
#   JENKINS_BUILD_USER       vain tämän käyttäjän käynnistämät buildit (tyhjä = kaikki)
#   JENKINS_DEPTH=3          kuinka syvälle kansioita (folder/multibranch) kuljetaan
#
#   SLIQTLY_DATA             Sliqtly-palvelimen datahakemisto; oletus haetaan
#                            /etc/sliqtly/sliqtly.env -> /var/lib/sliqtly (luetaan sudolla)
#   SLIQTLY_CHAT_FROM        vain tämän nimiset chat-viestit "Tehty"-listaan
#   SLIQTLY_URL, SLIQTLY_TOKEN   REST-varavaihtoehto (/api/v1/decks), jos kantaa ei löydy
#
#   GIT_REPOS                välilyönnillä erotettu lista repoja
#   GIT_SCAN_DIRS=/work/src  hakemistot joista kaikki repot etsitään (jos GIT_REPOS tyhjä)
#   GIT_SCAN_DEPTH=5
#   GIT_AUTHOR               oletus: globaali + jokaisen repon oma user.email; useita: "a@x\|b@y"
#   GIT_REFLOG=1             lisää reflog-tapahtumat (checkout, rebase, commit --amend ...)
#
#   WORK_DIRS=/work/src      files-lähteen hakemistot (välilyönnillä erotettu)
#   FILES_EXCLUDE            regex polkuihin joita ei lasketa
#
# Vaatii: bash, GNU date, python3. Valinnaiset: last, journalctl, curl, git.

set -u
FROM="${FROM:-2026-11-01}"
TO="${TO:-2026-11-10}"
SOURCES="${SOURCES:-login journal jenkins sliqtly git shell files}"
GAP_MIN="${GAP_MIN:-30}"
PAD_MIN="${PAD_MIN:-10}"
MIN_EVENTS_DAY="${MIN_EVENTS_DAY:-1}"
USER_NAME="${USER_NAME:-${USER:-$(id -un)}}"
VERBOSE=0
CSV=""
DUMP=""
TIMELINE="${TIMELINE:-1}"

usage() { sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM="$2"; shift 2;;
    --to) TO="$2"; shift 2;;
    --gap) GAP_MIN="$2"; shift 2;;
    --pad) PAD_MIN="$2"; shift 2;;
    --sources) SOURCES="$2"; shift 2;;
    --csv) CSV="$2"; shift 2;;
    --dump) DUMP="$2"; shift 2;;
    --no-timeline) TIMELINE=0; shift;;     # kirjoittaa raakatapahtumat tiedostoon
    -v|--verbose) VERBOSE=1; shift;;
    -h|--help) usage;;
    *) echo "Tuntematon argumentti: $1" >&2; exit 2;;
  esac
done

command -v python3 >/dev/null || { echo "python3 puuttuu" >&2; exit 1; }
START_EPOCH=$(date -d "$FROM 00:00:00" +%s) || exit 1
END_EPOCH=$(( $(date -d "$TO 00:00:00" +%s) + 86400 ))

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
EVENTS="$TMP/events.tsv"   # epoch<TAB>lähde<TAB>kuvaus
NOTES="$TMP/notes.tsv"     # epoch<TAB>lähde<TAB>mitä tehtiin ("Tehty"-lista)
: > "$EVENTS"; : > "$NOTES"

log()  { [ "$VERBOSE" = 1 ] && echo "[$1] $2" >&2; return 0; }
emit() { # emit lähde < "epoch<TAB>kuvaus" -rivit; suodattaa aikavälin
  awk -F'\t' -v s="$1" -v a="$START_EPOCH" -v b="$END_EPOCH" \
    '$1 ~ /^[0-9]+(\.[0-9]+)?$/ { e=int($1); if (e>=a && e<b) printf "%d\t%s\t%s\n", e, s, $2 }' >> "$EVENTS"
}
has() { case " $SOURCES " in *" $1 "*) return 0;; esac; return 1; }
count_src() { awk -F'\t' -v s="$1" '$2==s' "$EVENTS" | wc -l; }

# ---------------------------------------------------------------- login (wtmp)
src_login() {
  command -v last >/dev/null || { log login "last puuttuu"; return; }
  # --time-format iso: util-linux >= 2.25. Kirjautuminen ja (jos on) uloskirjautuminen.
  last -w --time-format iso -s "$FROM" -t "$(date -d "$TO +1 day" +%F)" "$USER_NAME" 2>/dev/null \
    | grep -Eo '[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}[+-][0-9:]{4,5}' \
    | while read -r ts; do printf '%s\tlogin/logout\n' "$(date -d "$ts" +%s)"; done | emit login
  # wtmp kiertää kuukausittain: luetaan myös /var/log/wtmp.1 jos olemassa
  if [ -r /var/log/wtmp.1 ]; then
    last -f /var/log/wtmp.1 -w --time-format iso -s "$FROM" -t "$(date -d "$TO +1 day" +%F)" "$USER_NAME" 2>/dev/null \
      | grep -Eo '[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}[+-][0-9:]{4,5}' \
      | while read -r ts; do printf '%s\tlogin/logout\n' "$(date -d "$ts" +%s)"; done | emit login
  fi
  log login "$(count_src login) tapahtumaa"
}

# ------------------------------------------------------------- journal
src_journal() {
  command -v journalctl >/dev/null || { log journal "journalctl puuttuu"; return; }
  # Huom: ilman sudoa/adm- tai systemd-journal-ryhmää näkyy vain oma user-journal.
  journalctl --since "$FROM 00:00:00" --until "$(date -d "$TO +1 day" +%F) 00:00:00" \
      -o short-unix --no-pager -q 2>/dev/null \
    | grep -Ei 'New session .* of user '"$USER_NAME"'|Removed session|Linux version|PM: suspend (entry|exit)|System is (suspending|resuming)|Lid (opened|closed)|unlocked|Unlocked|screen ?saver|gdm-password.*session opened|sudo:.*'"$USER_NAME"'' \
    | awk '{ t=$1; $1=""; sub(/^ /,""); printf "%s\t%s\n", t, substr($0,1,80) }' | emit journal
  log journal "$(count_src journal) tapahtumaa"
}

# ------------------------------------------------------------- jenkins
src_jenkins() {
  [ -n "${JENKINS_URL:-}" ] || { log jenkins "JENKINS_URL ei asetettu"; return; }
  command -v curl >/dev/null || { log jenkins "curl puuttuu"; return; }
  local depth="${JENKINS_DEPTH:-3}" inner="number,timestamp,duration,actions[causes[userId]]" tree
  tree="name,builds[$inner]"
  for _ in $(seq 2 "$depth"); do tree="name,builds[$inner],jobs[$tree]"; done
  local auth=()
  [ -n "${JENKINS_USER:-}" ] && auth=(-u "$JENKINS_USER:${JENKINS_TOKEN:-}")
  curl -fsS --connect-timeout 5 --max-time 60 "${auth[@]}" -G "${JENKINS_URL%/}/api/json" \
       --data-urlencode "tree=jobs[$tree]" -o "$TMP/jenkins.json" 2>"$TMP/jenkins.err" \
    || { log jenkins "haku epäonnistui: $(head -c 200 "$TMP/jenkins.err")"; return; }
  JBU="${JENKINS_BUILD_USER:-}" python3 - "$TMP/jenkins.json" <<'PY' | emit jenkins
import json, os, sys
want = os.environ.get("JBU", "")
def walk(jobs, path):
    for j in jobs or []:
        name = path + "/" + j.get("name", "?")
        for b in j.get("builds") or []:
            ts = b.get("timestamp")
            if not ts: continue
            users = {c.get("userId") for a in (b.get("actions") or []) if a
                     for c in (a.get("causes") or []) if c.get("userId")}
            if want and want not in users: continue
            s = ts // 1000
            print(f"{s}\t{name} #{b.get('number')} start")
            d = b.get("duration") or 0
            if d: print(f"{s + d // 1000}\t{name} #{b.get('number')} end")
        walk(j.get("jobs"), name)
walk(json.load(open(sys.argv[1])).get("jobs"), "")
PY
  log jenkins "$(count_src jenkins) tapahtumaa"
}

# ------------------------------------------------------------- sliqtly
# Sliqtly-palvelin (.deb, github.com/terotests/Sliqtly/mcp-go):
#   asetukset  /etc/sliqtly/sliqtly.env  (SLIQTLY_DATA=..., oletus /var/lib/sliqtly)
#   data       <data>/sliqtly.db   SQLite: docs(col,id,rev,doc JSON), chat_msgs(at ms, doc JSON),
#                                  file_refs(path, updated ms)
#              <data>/repo.git     bare Git -historia, jos `sliqtly-server git init` on ajettu:
#                                  commit ~1 min muokkauksen jälkeen, viesti esim.
#                                  'Deck "Nimi": text changed, 2 files added'
# Palvelu ajetaan DynamicUser-käyttäjänä, joten /var/lib/sliqtly -> /var/lib/private/sliqtly
# on vain rootin luettavissa: skripti lukee tiedostot sudolla (kysyy salasanan tarvittaessa)
# ja kopioi kannan väliaikaishakemistoon, eli käynnissä olevaa kantaa ei kosketa.
# Tapahtumat: dekkien created/updated (vain viimeisin muokkaus), chat-viestit, tiedostot,
# Git-commitit. "Tehty"-listaan: commit-viestit, uudet otsikot (deck.md:n +# -rivit), chat.
# Jos kantaa ei löydy, kokeillaan REST-rajapintaa GET /api/v1/decks (Bearer-token).
#   SLIQTLY_DATA       datahakemisto (ohittaa automaattihaun)
#   SLIQTLY_CHAT_FROM  vain tämän nimiset chat-kirjoittajat "Tehty"-listaan (tyhjä = kaikki ihmiset)
#   SLIQTLY_URL, SLIQTLY_TOKEN   REST-varavaihtoehto
SQ_SUDO=""
sq() { $SQ_SUDO "$@"; }
sliqtly_find_data() {
  if [ -n "${SLIQTLY_DATA:-}" ]; then echo "$SLIQTLY_DATA"; return; fi
  local env d
  for env in /etc/sliqtly/sliqtly.env /etc/slitqtly/sliqtly.env; do
    d=$(sq cat "$env" 2>/dev/null | sed -nE 's/^[[:space:]]*SLIQTLY_DATA=["]?([^"]+)["]?.*/\1/p' | tail -1)
    [ -n "$d" ] && sq test -e "$d/sliqtly.db" && { echo "$d"; return; }
  done
  for d in /var/lib/sliqtly /var/lib/private/sliqtly /etc/sliqtly /etc/slitqtly "$HOME/.sliqtly" "$HOME/sliqtly"; do
    sq test -e "$d/sliqtly.db" 2>/dev/null && { echo "$d"; return; }
  done
}
src_sliqtly() {
  local data
  data=$(sliqtly_find_data)
  if [ -z "$data" ] && [ "$(id -u)" != 0 ] && command -v sudo >/dev/null; then
    log sliqtly "kantaa ei näy ilman rootia, kokeillaan sudolla"
    SQ_SUDO="sudo"; data=$(sliqtly_find_data)
  fi
  if [ -n "$data" ] && ! sq test -r "$data/sliqtly.db" 2>/dev/null && [ "$(id -u)" != 0 ]; then SQ_SUDO="sudo"; fi
  if [ -n "$data" ]; then
    log sliqtly "data: $data${SQ_SUDO:+ (sudo)}"
    mkdir -p "$TMP/sq"
    local f
    for f in sliqtly.db sliqtly.db-wal; do
      sq test -e "$data/$f" && sq cat "$data/$f" > "$TMP/sq/$f"
    done
    : > "$TMP/sq/git.log"; : > "$TMP/sq/git.diff"
    if sq test -d "$data/repo.git" && command -v git >/dev/null; then
      local G=(git -c safe.directory='*' --git-dir="$data/repo.git")
      sq "${G[@]}" log --all --since="@$START_EPOCH" --until="@$END_EPOCH" \
        --format='%x1e%at%x1f%B' > "$TMP/sq/git.log" 2>/dev/null
      sq "${G[@]}" log --all --since="@$START_EPOCH" --until="@$END_EPOCH" \
        --format='@@C %at' -p --unified=0 --no-color -- 'decks/*/deck.md' > "$TMP/sq/git.diff" 2>/dev/null
      log sliqtly "repo.git: $(grep -c $'\x1e' "$TMP/sq/git.log") committia"
    else
      log sliqtly "ei repo.git-historiaa (sliqtly-server git init), vain viimeisimmät muokkausajat"
    fi
    CHAT_FROM="${SLIQTLY_CHAT_FROM:-}" python3 - "$TMP/sq" "$NOTES" <<'PY' | emit sliqtly
import json, os, re, sqlite3, sys
from datetime import datetime
d, notes_path = sys.argv[1], sys.argv[2]
notes = open(notes_path, "a")
chat_from = os.environ.get("CHAT_FROM", "").lower()
def ts(v):
    if isinstance(v, (int, float)): return int(v / 1000 if v > 1e11 else v)
    if isinstance(v, str):
        try: return int(datetime.fromisoformat(v.replace("Z", "+00:00")).timestamp())
        except ValueError: return None
    if isinstance(v, dict):   # Firestore-tyylinen {"_seconds":..} tai {"$t":..}
        for k in ("_seconds", "seconds"):
            if k in v: return int(v[k])
def ev(t, desc): print(f"{t}\t{desc}")
def note(t, txt): notes.write(f"{t}\tsliqtly\t{txt[:160]}\n")
def clean(s): return re.sub(r"\s+", " ", s or "").strip()
names, rooms = {}, {}
if os.path.exists(f"{d}/sliqtly.db"):
    db = sqlite3.connect(f"{d}/sliqtly.db")
    tables = {r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    if "docs" in tables:
        for col, id_, doc in db.execute("SELECT col, id, doc FROM docs WHERE col IN ('shares','rooms')"):
            try: o = json.loads(doc)
            except ValueError: continue
            name = clean(o.get("name") or o.get("title") or id_)[:60]
            (names if col == "shares" else rooms)[id_] = name
            for k in ("created", "updated"):
                t = ts(o.get(k))
                if t:
                    ev(t, f"{col} {k}: {name}")
                    if col == "shares": note(t, f'{"Luotu" if k == "created" else "Muokattu viimeksi"}: "{name}"')
    if "chat_msgs" in tables:
        for room, at, doc in db.execute("SELECT room, at, doc FROM chat_msgs"):
            try: o = json.loads(doc)
            except ValueError: continue
            t = ts(at); fr = o.get("from") or {}
            if not t or o.get("deleted"): continue
            who = fr.get("name") or fr.get("user") or "?"
            ev(t, f"chat [{rooms.get(room, room)}] {who}: {clean(o.get('text'))[:90]}")
            if fr.get("kind") == "bot": continue
            if chat_from and chat_from not in who.lower(): continue
            txt = clean(o.get("text"))
            if txt: note(t, f"Chat [{rooms.get(room, room)}] {who}: {txt}")
    if "file_refs" in tables:
        for path, upd in db.execute("SELECT path, updated FROM file_refs"):
            t = ts(upd)
            if t: ev(t, f"file {path}")
# Git: commit-viestit ("Deck \"X\": text changed ...")
raw = open(f"{d}/git.log", errors="replace").read() if os.path.exists(f"{d}/git.log") else ""
for rec in raw.split("\x1e"):
    if "\x1f" not in rec: continue
    t, body = rec.split("\x1f", 1); t = int(t)
    lines = [l.strip() for l in body.strip().splitlines() if l.strip()]
    ev(t, "repo.git " + (lines[0] if lines else ""))
    for l in lines:
        if l.endswith("decks changed"): continue
        note(t, l)
# Git: lisätyt otsikot dekkeihin
t, deck = None, None
if os.path.exists(f"{d}/git.diff"):
    for l in open(f"{d}/git.diff", errors="replace"):
        if l.startswith("@@C "): t = int(l[4:]); continue
        m = re.match(r"\+\+\+ b/decks/([^/]+)/deck\.md", l)
        if m: deck = names.get(m.group(1), m.group(1)); continue
        m = re.match(r"\+(#{1,3})\s+(.+)", l)
        if m and t: note(t, f'Otsikko lisätty "{deck}": {clean(m.group(2))}')
PY
  else
    log sliqtly "sliqtly.db ei löytynyt (/etc/sliqtly/sliqtly.env, /var/lib/sliqtly); kokeillaan REST-rajapintaa"
    sliqtly_http
  fi
  log sliqtly "$(count_src sliqtly) tapahtumaa"
}
sliqtly_http() {
  command -v curl >/dev/null || return
  local url="${SLIQTLY_URL:-http://localhost:8080}" tok="${SLIQTLY_TOKEN:-}" hdr=()
  [ -z "$tok" ] && tok=$(sq cat /var/lib/sliqtly/access-token 2>/dev/null)
  [ -n "$tok" ] && hdr=(-H "Authorization: Bearer $tok")
  curl -fsS --max-time 20 "${hdr[@]}" "${url%/}/api/v1/decks" -o "$TMP/sq_decks.json" 2>"$TMP/sq_err" \
    || { log sliqtly "GET /api/v1/decks epäonnistui: $(head -c 200 "$TMP/sq_err")"; return; }
  python3 - "$TMP/sq_decks.json" "$NOTES" <<'PY' | emit sliqtly
import json, sys
notes = open(sys.argv[2], "a")
for x in json.load(open(sys.argv[1])).get("decks", []):
    u = x.get("updated")
    if isinstance(u, (int, float)):
        t = int(u / 1000 if u > 1e11 else u)
        print(f"{t}\tdeck updated: {x.get('name')}")
        notes.write(f"{t}\tsliqtly\tMuokattu viimeksi: \"{x.get('name')}\"\n")
PY
}

# ------------------------------------------------------------- git
src_git() {
  command -v git >/dev/null || { log git "git puuttuu"; return; }
  local list="$TMP/git_repos.txt" r d
  : > "$list"
  if [ -n "${GIT_REPOS:-}" ]; then
    for r in $GIT_REPOS; do echo "$r" >> "$list"; done
  else
    for d in ${GIT_SCAN_DIRS:-/work/src}; do
      [ -d "$d" ] || { log git "$d puuttuu"; continue; }
      # .git voi olla hakemisto tai (worktree/submodule) tiedosto; node_modules ohitetaan
      find "$d" -maxdepth "${GIT_SCAN_DEPTH:-5}" \( -name node_modules -prune \) -o \
           -name .git -print -prune 2>/dev/null | sed 's|/\.git$||' >> "$list"
    done
  fi
  sort -u -o "$list" "$list"
  # author: GIT_AUTHOR tai kaikkien löytyneiden repojen user.email-arvot + globaali
  local author="${GIT_AUTHOR:-}"
  if [ -z "$author" ]; then
    author=$( { git config --global user.email
                while IFS= read -r r; do git -C "$r" config user.email; done < "$list"; } 2>/dev/null \
              | sort -u | grep -v '^$' | paste -sd'|' -)
    author=${author//|/\\|}
  fi
  local n=0 c name
  while IFS= read -r r; do
    git -C "$r" rev-parse --git-dir >/dev/null 2>&1 || continue
    n=$((n+1)); local base="${GIT_SCAN_DIRS:-/work/src}"; name=${r#"${base%% *}/"}; [ "$name" = "$r" ] && name=$(basename "$r")
    # --since/--until on committer-aika; emit suodattaa author-ajan (%at) vielä välille
    git -C "$r" log --all --no-merges ${author:+--author="$author"} \
        --since="@$((START_EPOCH - 86400*30))" --format="%at%x09%h%x09%s" 2>/dev/null \
      | awk -F'\t' -v a="$START_EPOCH" -v b="$END_EPOCH" '$1>=a && $1<b' | cut -c1-200 > "$TMP/git_c.tsv"
    c=$(wc -l < "$TMP/git_c.tsv")
    if [ "$c" -gt 0 ]; then
      log git "  $name: $c committia"
      awk -F'\t' -v n="$name" '{ printf "%s\t%s commit %s %s\n", $1, n, $2, $3 }' "$TMP/git_c.tsv" | emit git
      awk -F'\t' -v n="$name" '{ printf "%s\tgit\t%s: %s\n", $1, n, $3 }' "$TMP/git_c.tsv" >> "$NOTES"
    fi
    if [ "${GIT_REFLOG:-1}" = 1 ]; then
      # reflog on paikallinen eli kertoo omasta toiminnasta tällä koneella (checkout, pull, rebase...)
      git -C "$r" reflog --all --date=unix --format="%gd%x09%gs" 2>/dev/null \
        | sed -nE "s/^[^@]*@\{([0-9]+)\}\t(.*)$/\1\t$name reflog \2/p" | cut -c1-120 | emit git
    fi
  done < "$list"
  log git "$n repoa, author='${author:-*}', $(count_src git) tapahtumaa"
}

# ------------------------------------------------------------- shell history
src_shell() {
  # zsh EXTENDED_HISTORY: ": 1699999999:0;komento"
  for f in "$HOME/.zsh_history" "${HISTFILE:-}"; do
    [ -r "$f" ] && sed -nE 's/^: ([0-9]{9,11}):[0-9]+;.*/\1\tzsh/p' "$f" 2>/dev/null | emit shell
  done
  # bash HISTTIMEFORMAT: rivit "#1699999999"
  [ -r "$HOME/.bash_history" ] && sed -nE 's/^#([0-9]{9,11})$/\1\tbash/p' "$HOME/.bash_history" | emit shell
  log shell "$(count_src shell) tapahtumaa (bash tarvitsee HISTTIMEFORMAT-asetuksen)"
}

# ------------------------------------------------------------- files
src_files() {
  local ex="${FILES_EXCLUDE:-/(node_modules|\.git|\.cache|dist|build|target|__pycache__|\.venv|venv|\.next|coverage)/}"
  for d in ${WORK_DIRS:-/work/src}; do
    [ -d "$d" ] || { log files "$d puuttuu"; continue; }
    find "$d" -type f -newermt "$FROM" ! -newermt "$(date -d "$TO +1 day" +%F)" -printf '%T@\t%p\n' 2>/dev/null \
      | grep -Ev "$ex" | emit files
  done
  log files "$(count_src files) tapahtumaa"
}

for s in $SOURCES; do
  if declare -F "src_$s" >/dev/null; then "src_$s" || log "$s" "virhe, ohitetaan"
  else echo "Tuntematon lähde: $s" >&2; fi
done

[ -n "$DUMP" ] && sort -n "$EVENTS" > "$DUMP"

GAP_MIN="$GAP_MIN" PAD_MIN="$PAD_MIN" MIN_EV="$MIN_EVENTS_DAY" CSV="$CSV" \
FROM="$FROM" TO="$TO" SOURCES="$SOURCES" VERBOSE="$VERBOSE" NOTES_MAX="${NOTES_MAX:-25}" \
TIMELINE="$TIMELINE" WORK_DIRS="${WORK_DIRS:-/work/src}" \
python3 - "$EVENTS" "$NOTES" <<'PY'
import os, sys, csv
from datetime import datetime, date, timedelta
from collections import defaultdict
gap = float(os.environ["GAP_MIN"]) * 60
pad = float(os.environ["PAD_MIN"]) * 60
min_ev = int(os.environ["MIN_EV"])
sources = os.environ["SOURCES"].split()
verbose = os.environ["VERBOSE"] == "1"
days = defaultdict(list)
for line in open(sys.argv[1]):
    p = line.rstrip("\n").split("\t", 2)
    if len(p) < 2: continue
    t = int(p[0]); d = datetime.fromtimestamp(t).date()
    days[d].append((t, p[1], p[2] if len(p) > 2 else ""))

def hm(sec): sec = int(round(sec / 60)); return f"{sec // 60}:{sec % 60:02d}"
rows = []
d, end = date.fromisoformat(os.environ["FROM"]), date.fromisoformat(os.environ["TO"])
while d <= end:
    ev = sorted(days.get(d, []))
    if len(ev) < min_ev:
        rows.append(dict(day=d, n=0)); d += timedelta(days=1); continue
    sessions, s0, prev = [], ev[0][0], ev[0][0]
    for t, *_ in ev[1:]:
        if t - prev > gap: sessions.append((s0, prev)); s0 = t
        prev = t
    sessions.append((s0, prev))
    active = sum(b - a + pad for a, b in sessions)
    cnt = defaultdict(int)
    for _, s, _d in ev: cnt[s] += 1
    rows.append(dict(day=d, n=len(ev), first=ev[0][0], last=ev[-1][0],
                     span=ev[-1][0] - ev[0][0], active=active, sessions=sessions, cnt=cnt))
    d += timedelta(days=1)

wd = ["ma", "ti", "ke", "to", "pe", "la", "su"]
fmt = lambda t: datetime.fromtimestamp(t).strftime("%H:%M")
print(f"Työaika-arvio {os.environ['FROM']} .. {os.environ['TO']}  (gap {int(gap/60)} min, pad {int(pad/60)} min)\n")
head = f"{'Päivä':<14}{'Alku':>6}{'Loppu':>7}{'Span':>7}{'Arvio':>7}{'Jaksot':>7}  " + " ".join(f"{s[:7]:>7}" for s in sources)
print(head); print("-" * len(head))
tot_a = tot_s = 0
for r in rows:
    lab = f"{r['day']} {wd[r['day'].weekday()]}"
    if not r["n"]:
        print(f"{lab:<14}{'-':>6}"); continue
    tot_a += r["active"]; tot_s += r["span"]
    print(f"{lab:<14}{fmt(r['first']):>6}{fmt(r['last']):>7}{hm(r['span']):>7}{hm(r['active']):>7}{len(r['sessions']):>7}  "
          + " ".join(f"{r['cnt'].get(s, 0):>7}" for s in sources))
    if verbose:
        print("              jaksot: " + ", ".join(f"{fmt(a)}-{fmt(b)}" for a, b in r["sessions"]))
print("-" * len(head))
print(f"{'Yhteensä':<14}{'':>6}{'':>7}{hm(tot_s):>7}{hm(tot_a):>7}")
notes = defaultdict(list)
for line in open(sys.argv[2]):
    p = line.rstrip("\n").split("\t", 2)
    if len(p) == 3 and p[0].isdigit():
        notes[datetime.fromtimestamp(int(p[0])).date()].append((int(p[0]), p[1], p[2]))
nmax = int(os.environ["NOTES_MAX"])
if notes and nmax > 0:
    print("\nTehty (git-commitit, Sliqtly-muutokset ja chat):")
    for day in sorted(notes):
        if not (date.fromisoformat(os.environ["FROM"]) <= day <= end): continue
        print(f"\n{day} {wd[day.weekday()]}")
        seen, items = set(), []
        for t, s, txt in sorted(notes[day]):
            if (s, txt) in seen: continue
            seen.add((s, txt)); items.append((t, s, txt))
        for t, s, txt in items[:nmax]:
            print(f"  {fmt(t)} [{s}] {txt}")
        if len(items) > nmax: print(f"  ... +{len(items) - nmax} lisää (NOTES_MAX)")
# Aikajana: jokainen tapahtuma jaksoittain. Peräkkäiset samanlaiset tapahtumat
# (sama lähde ja kuvaus, tiedostoilla sama hakemisto) yhdistetään yhdelle riville.
if os.environ["TIMELINE"] == "1":
    import os.path
    roots = [r.rstrip("/") + "/" for r in os.environ["WORK_DIRS"].split()]
    def key(src, desc):
        if src == "files":
            for r in roots:
                if desc.startswith(r): desc = desc[len(r):]; break
            return ("files", "muokattu: " + (os.path.dirname(desc) or "."), os.path.basename(desc))
        return (src, desc, None)
    print("\n" + "=" * 70 + "\nAikajana (mitä tehtiin milloinkin)\n" + "=" * 70)
    for r in rows:
        if not r["n"]: continue
        ev = sorted(days[r["day"]])
        print(f"\n{r['day']} {wd[r['day'].weekday()]}   arvio {hm(r['active'])}")
        for a, b in r["sessions"]:
            print(f"  -- jakso {fmt(a)}-{fmt(b)} ({hm(b - a + pad)}) --")
            run = None
            def flush(run):
                if not run: return
                t0, t1, (src, desc, _), n, files = run
                when = fmt(t0) if fmt(t0) == fmt(t1) else f"{fmt(t0)}-{fmt(t1)}"
                extra = ""
                if src == "files":
                    shown = sorted(set(files))
                    extra = ": " + ", ".join(shown[:4]) + (f" +{len(shown) - 4}" if len(shown) > 4 else "")
                elif n > 1: extra = f" (x{n})"
                print(f"  {when:<11} [{src}] {desc[:110]}{extra}")
            for t, src, desc in ev:
                if t < a or t > b: continue
                k = key(src, desc)
                if run and run[2][:2] == k[:2]:
                    run[1] = t; run[3] += 1
                    if k[2]: run[4].append(k[2])
                else:
                    flush(run); run = [t, t, k, 1, [k[2]] if k[2] else []]
            flush(run)
print("\nArvio = yhtenäisten aktiivisuusjaksojen summa; Span = päivän ensimmäinen..viimeinen tapahtuma (yläraja).")
if os.environ["CSV"]:
    with open(os.environ["CSV"], "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["date", "first", "last", "span_h", "estimate_h", "sessions"] + sources)
        for r in rows:
            if not r["n"]: w.writerow([r["day"]] + [""] * 5 + [0] * len(sources)); continue
            w.writerow([r["day"], fmt(r["first"]), fmt(r["last"]), round(r["span"] / 3600, 2),
                        round(r["active"] / 3600, 2), len(r["sessions"])] + [r["cnt"].get(s, 0) for s in sources])
    print(f"CSV: {os.environ['CSV']}")
PY
