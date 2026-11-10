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
#   ./hours.sh --html raportti.html   # HTML-raportti (oletus hours-report.html, --no-html pois)
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
# Kiinteät merkinnät (tiedossa olevat työt, lisätään laskettujen päälle).
# Muoto: PÄIVÄ|HH:MM-HH:MM|kuvaus  tai  PÄIVÄ|tunnit h|kuvaus
# Kellonajalliset yhdistetään laskettuihin 30 min jaksoihin (ei tuplalaskentaa),
# pelkät tuntimäärät lisätään päivän arvioon sellaisenaan.
# Korvaa tiedostolla: MANUAL_FILE=omat.txt, tai tyhjennä: MANUAL_FILE=/dev/null
# Repo on julkinen, joten tähän vain yleiset kuvaukset. Nimet ym. omaan tiedostoon:
# hours-manual.txt (nykyhakemisto) tai ~/.config/hours/manual.txt luetaan automaattisesti.
DEFAULT_MANUAL='
2026-11-04|08:30-11:30|Morning status check and larger team meeting; building a new build, bug investigation
2026-11-05|09:00-13:00|At the office: reviewing new merge requests
2026-11-05|14:00-15:00|At the office: internal communication with the customer
2026-11-05|15:00-17:00|At the office: build work, bug fixing, project communication
2026-11-06|09:00-13:00|IPC refactor work estimate
2026-11-07|4h|Build preparation, continued IPC refactor work estimate
2026-11-08|1h|Building the build
2026-11-09|3.5h|Project updates, status, reading updates
2026-11-09|3h|Night: building a new build
'
for f in ./hours-manual.txt "$HOME/.config/hours/manual.txt"; do
  [ -z "${MANUAL_FILE:-}" ] && [ -r "$f" ] && MANUAL_FILE="$f"
done
if [ -n "${MANUAL_FILE:-}" ]; then MANUAL=$(cat "$MANUAL_FILE"); else MANUAL="${MANUAL-$DEFAULT_MANUAL}"; fi
HTML="${HTML-hours-report.html}"

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
    --no-timeline) TIMELINE=0; shift;;
    --html) HTML="$2"; shift 2;;
    --no-html) HTML=""; shift;;     # kirjoittaa raakatapahtumat tiedostoon
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
        | sed -nE 's/^[^@]*@\{([0-9]+)\}\t(.*)$/\1\t\2/p' \
        | sort -u | awk -F'\t' -v n="$name" '$2 !~ /^commit/ { printf "%s\t%s reflog %s\n", $1, n, $2 }' | cut -c1-140 | emit git
    fi
  done < "$list"
  log git "$n repoa, author='${author:-*}', $(count_src git) tapahtumaa"
}

# ------------------------------------------------------------- network (toimisto vai etä?)
# Ei lasketa aktiivisuudeksi, vaan näytetään päivittäin mitä verkkoja käytettiin.
# NetworkManagerin loki: "policy: set 'Yhteys' (enp0s31f6) as default ...", DHCP-osoitteet,
# WiFi-verkon nimi. Liitäntä e* = kaapeli (enx* usein USB-C-telakka), w* = WiFi.
#   NETWORK=0      ohittaa
#   OFFICE_MATCH   regex, joka tunnistaa toimistoverkon (yhteyden nimi, SSID, liitäntä tai
#                  IP-alku), esim. OFFICE_MATCH='enx|Corp-WLAN|^10\.20\.'
net_collect() {
  NET="$TMP/net.tsv"; : > "$NET"
  [ "${NETWORK:-1}" = 1 ] || return
  local since="$FROM 00:00:00" until; until="$(date -d "$TO +1 day" +%F) 00:00:00"
  { journalctl -u NetworkManager --since "$since" --until "$until" -o short-unix --no-pager -q 2>/dev/null
    # vanhemmat/ryhmättömät: syslog (adm-ryhmä)
    for f in /var/log/syslog /var/log/syslog.1; do
      [ -r "$f" ] && grep -a NetworkManager "$f" | while IFS= read -r l; do
        t=$(date -d "$(echo "$l" | cut -c1-32 | grep -oE '^[0-9]{4}-[0-9-]{5}T[0-9:.+-]+|^[A-Z][a-z]{2} [ 0-9]{2} [0-9:]{8}')" +%s 2>/dev/null) && echo "$t $l"
      done
    done; } > "$TMP/nm.log"
  if [ ! -s "$TMP/nm.log" ] && [ "$(id -u)" != 0 ] && command -v sudo >/dev/null; then
    log network "NetworkManager-loki tyhjä ilman rootia, kokeillaan sudolla"
    sudo journalctl -u NetworkManager --since "$since" --until "$until" -o short-unix --no-pager -q > "$TMP/nm.log" 2>/dev/null
  fi
  python3 - "$TMP/nm.log" "$START_EPOCH" "$END_EPOCH" > "$NET" <<'PY'
import re, sys
a, b = int(sys.argv[2]), int(sys.argv[3])
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"(\d+)(?:\.\d+)?\s", l)
    if not m: continue
    t = int(m.group(1))
    if not a <= t < b: continue
    m = re.search(r"policy: set '([^']+)' \(([^)]+)\) as default for IPv4", l)
    if m: print(f"{t}\t{m.group(2)}\tconn\t{m.group(1)}"); continue
    m = re.search(r"dhcp4 \(([^)]+)\):.*(?:ip_address => '|address=)([0-9.]+)", l)
    if m: print(f"{t}\t{m.group(1)}\tip\t{m.group(2)}"); continue
    m = re.search(r"device \(([^)]+)\):.*(?:Connected to wireless network|SSID) ['\"]?([^'\"]+)", l)
    if m: print(f"{t}\t{m.group(1)}\tssid\t{m.group(2).strip()}"); continue
    m = re.search(r"device \(([^)]+)\): Activation: successful", l)
    if m: print(f"{t}\t{m.group(1)}\tup\t")
PY
  log network "$(wc -l < "$NET") verkkotapahtumaa"
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

net_collect
[ -n "$DUMP" ] && sort -n "$EVENTS" > "$DUMP"

GAP_MIN="$GAP_MIN" PAD_MIN="$PAD_MIN" MIN_EV="$MIN_EVENTS_DAY" CSV="$CSV" \
FROM="$FROM" TO="$TO" SOURCES="$SOURCES" VERBOSE="$VERBOSE" NOTES_MAX="${NOTES_MAX:-25}" \
MANUAL="$MANUAL" SLOT_MIN="${SLOT_MIN:-30}" TIMELINE="$TIMELINE" HTML="$HTML" WORK_DIRS="${WORK_DIRS:-/work/src}" \
OFFICE_MATCH="${OFFICE_MATCH:-}" python3 - "$EVENTS" "$NOTES" "$NET" <<'PY'
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
SLOT = int(os.environ.get("SLOT_MIN", "30")) * 60
def midnight(d): return datetime(d.year, d.month, d.day).timestamp()
# Kiinteät (käsin tiedossa olevat) merkinnät: "PVM|HH:MM-HH:MM tai N h|kuvaus"
manual = defaultdict(list)   # päivä -> [(alku_ts|None, loppu_ts|None, tunnit, kuvaus)]
for ln in os.environ.get("MANUAL", "").splitlines():
    ln = ln.split("#", 1)[0].strip() if not ln.strip().startswith("20") else ln.strip()
    if not ln: continue
    try:
        dstr, when, desc = [x.strip() for x in ln.split("|", 2)]
        md = date.fromisoformat(dstr); m0 = midnight(md)
        if "-" in when and ":" in when:
            a_, b_ = when.split("-")
            ta = m0 + int(a_.split(":")[0]) * 3600 + int(a_.split(":")[1]) * 60
            tb = m0 + int(b_.split(":")[0]) * 3600 + int(b_.split(":")[1]) * 60
            if tb <= ta: tb += 86400
            manual[md].append((ta, tb, (tb - ta) / 3600, desc))
        else:
            manual[md].append((None, None, float(when.lower().rstrip("h ").replace(",", ".")), desc))
    except Exception as e:
        print(f"Kiinteä merkintä ohitettu ({e}): {ln}", file=sys.stderr)
import re as _re
net = defaultdict(dict)   # päivä -> {(liitäntä, yhteys/SSID): [ensim., viim., ipt]}
_ip = {}
for ln in open(sys.argv[3]):
    p = ln.rstrip("\n").split("\t")
    if len(p) < 4: continue
    t, iface, kind, val = int(p[0]), p[1], p[2], p[3]
    d_ = datetime.fromtimestamp(t).date()
    k = (iface, val if kind in ("conn", "ssid") else "")
    if kind == "ip": _ip[(d_, iface)] = val; k = (iface, "")
    e = net[d_].setdefault(k, [t, t]); e[1] = max(e[1], t); e[0] = min(e[0], t)
office_re = _re.compile(os.environ["OFFICE_MATCH"]) if os.environ.get("OFFICE_MATCH") else None
def net_summary(d_):
    out, office = [], False
    names = {i for (i, v) in net.get(d_, {}) if v}
    for (iface, val), (t0, t1) in sorted(net.get(d_, {}).items(), key=lambda x: x[1][0]):
        if not val and iface in names: continue
        typ = "kaapeli" if iface.startswith("e") else "WiFi" if iface.startswith("w") else iface
        ip = _ip.get((d_, iface), "")
        txt = f"{typ} {iface}" + (f" '{val}'" if val else "") + (f" {ip}" if ip else "") + f" {datetime.fromtimestamp(t0).strftime('%H:%M')}"
        if office_re and any(office_re.search(x) for x in (iface, val, ip) if x): office = True; txt += " [toimisto]"
        out.append(txt)
    return out, office
rows = []
d, end = date.fromisoformat(os.environ["FROM"]), date.fromisoformat(os.environ["TO"])
while d <= end:
    ev = sorted(days.get(d, []))
    if len(ev) < min_ev: ev = []
    man = manual.get(d, [])
    if not ev and not man:
        rows.append(dict(day=d, n=0)); d += timedelta(days=1); continue
    m0 = midnight(d)
    sessions = []
    if ev:
        s0 = prev = ev[0][0]
        for t, *_ in ev[1:]:
            if t - prev > gap: sessions.append((s0, prev)); s0 = t
            prev = t
        sessions.append((s0, prev))
    # 30 min tarkkuus: jokainen kellon puolituntinen, jossa on yksikin tapahtuma, = 30 min
    slots = {int((t - m0) // SLOT) for t, *_ in ev}
    mslots = set()
    for ta, tb, _, _ in man:
        if ta is not None:
            mslots |= set(range(int((ta - m0) // SLOT), int((tb - m0 - 1) // SLOT) + 1))
    untimed = sum(h_ for ta, _, h_, _ in man if ta is None) * 3600
    comp = len(slots) * SLOT
    est = len(slots | mslots) * SLOT + untimed       # päällekkäisiä puolitunteja ei lasketa kahdesti
    times = [t for t, *_ in ev] + [x for ta, tb, _, _ in man if ta is not None for x in (ta, tb)]
    cnt = defaultdict(int)
    for _, s_, _d in ev: cnt[s_] += 1
    if man: cnt["manual"] = len(man)
    rows.append(dict(day=d, n=len(ev) + len(man), first=min(times) if times else None,
                     last=max(times) if times else None,
                     span=(max(times) - min(times)) if times else 0, active=est, comp=comp,
                     fixed=est - comp, slots=slots, mslots=mslots, man=man, sessions=sessions, cnt=cnt))
    d += timedelta(days=1)

wd = ["ma", "ti", "ke", "to", "pe", "la", "su"]
fmt = lambda t: datetime.fromtimestamp(t).strftime("%H:%M") if t is not None else "-"
if manual and "manual" not in sources: sources.append("manual")
print(f"Työaika-arvio {os.environ['FROM']} .. {os.environ['TO']}  ({SLOT//60} min tarkkuus; kiinteät merkinnät lisätty)\n")
head = f"{'Päivä':<14}{'Alku':>6}{'Loppu':>7}{'Span':>7}{'Lask.':>7}{'Kiint.':>7}{'Arvio':>7}  " + " ".join(f"{s[:7]:>7}" for s in sources)
print(head); print("-" * len(head))
tot_a = tot_s = tot_c = tot_f = 0
for r in rows:
    lab = f"{r['day']} {wd[r['day'].weekday()]}"
    if not r["n"]:
        print(f"{lab:<14}{'-':>6}"); continue
    tot_a += r["active"]; tot_s += r["span"]; tot_c += r["comp"]; tot_f += r["fixed"]
    print(f"{lab:<14}{fmt(r['first']):>6}{fmt(r['last']):>7}{hm(r['span']):>7}{hm(r['comp']):>7}{('+' + hm(r['fixed'])) if r['fixed'] else '':>7}{hm(r['active']):>7}  "
          + " ".join(f"{r['cnt'].get(s, 0):>7}" for s in sources))
    nl, _o = net_summary(r["day"])
    if nl: print("              verkko: " + "; ".join(nl))
    if verbose:
        print("              jaksot: " + ", ".join(f"{fmt(a)}-{fmt(b)}" for a, b in r["sessions"]))
print("-" * len(head))
print(f"{'Yhteensä':<14}{'':>6}{'':>7}{hm(tot_s):>7}{hm(tot_c):>7}{'+' + hm(tot_f):>7}{hm(tot_a):>7}")
notes = defaultdict(list)
for line in open(sys.argv[2]):
    p = line.rstrip("\n").split("\t", 2)
    if len(p) == 3 and p[0].isdigit():
        notes[datetime.fromtimestamp(int(p[0])).date()].append((int(p[0]), p[1], p[2]))
nmax = int(os.environ["NOTES_MAX"])
start_d = date.fromisoformat(os.environ["FROM"])
# "Tehty": duplikaatit pois
for md, items in manual.items():
    for ta, tb, h_, desc in items:
        t = ta if ta is not None else midnight(md)
        lab_ = f"{fmt(ta)}-{fmt(tb)}" if ta is not None else f"{h_:g} h"
        notes[md].append((int(t), "manual", f"{desc} ({lab_})"))
def tlab(t, s, txt):   # kellonajaton kiinteä merkintä: ei kellonaikaa
    return "  -  " if s == "manual" and txt.endswith(" h)") else fmt(t)
done = {}
for day in sorted(notes):
    if not (start_d <= day <= end): continue
    seen, items = set(), []
    for t, s, txt in sorted(notes[day]):
        if (s, txt) in seen: continue
        seen.add((s, txt)); items.append((t, s, txt))
    done[day] = items
# Aikajana: jokainen tapahtuma jaksoittain. Peräkkäiset samanlaiset tapahtumat
# (sama lähde ja kuvaus, tiedostoilla sama hakemisto) yhdistetään yhdelle riville.
import os.path
roots = [x.rstrip("/") + "/" for x in os.environ["WORK_DIRS"].split()]
def key(src, desc):
    if src == "files":
        for x in roots:
            if desc.startswith(x): desc = desc[len(x):]; break
        return ("files", "muokattu: " + (os.path.dirname(desc) or "."), os.path.basename(desc))
    return (src, desc, None)
def item(run):
    t0, t1, (src, desc, _), n, files = run
    when = fmt(t0) if fmt(t0) == fmt(t1) else f"{fmt(t0)}-{fmt(t1)}"
    extra = ""
    if src == "files":
        shown = sorted(set(files))
        extra = ": " + ", ".join(shown[:4]) + (f" +{len(shown) - 4}" if len(shown) > 4 else "")
    elif n > 1: extra = f" (x{n})"
    return (when, src, desc[:140] + extra)
tl = {}
for r in rows:
    if not r["n"]: continue
    ev = sorted(days[r["day"]]); out = []
    for a, b in r["sessions"]:
        items, run = [], None
        for t, src, desc in ev:
            if t < a or t > b: continue
            k = key(src, desc)
            if run and run[2][:2] == k[:2]:
                run[1] = t; run[3] += 1
                if k[2]: run[4].append(k[2])
            else:
                if run: items.append(item(run))
                run = [t, t, k, 1, [k[2]] if k[2] else []]
        if run: items.append(item(run))
        out.append((a, b, items))
    if r.get("man"):
        out.insert(0, (None, None, [((f"{fmt(ta)}-{fmt(tb)}" if ta is not None else f"{h_:g} h"), "manual", desc)
                                     for ta, tb, h_, desc in r["man"]]))
    tl[r["day"]] = out

if done and nmax > 0:
    print("\nTehty (git-commitit, Sliqtly-muutokset ja chat):")
    for day, items in done.items():
        print(f"\n{day} {wd[day.weekday()]}")
        for t, s, txt in items[:nmax]: print(f"  {tlab(t, s, txt)} [{s}] {txt}")
        if len(items) > nmax: print(f"  ... +{len(items) - nmax} lisää (NOTES_MAX)")
if os.environ["TIMELINE"] == "1":
    print("\n" + "=" * 70 + "\nAikajana (mitä tehtiin milloinkin)\n" + "=" * 70)
    for r in rows:
        if not r["n"]: continue
        print(f"\n{r['day']} {wd[r['day'].weekday()]}   arvio {hm(r['active'])}")
        for a, b, items in tl[r["day"]]:
            print("  -- kiinteät merkinnät --" if a is None else f"  -- jakso {fmt(a)}-{fmt(b)} --")
            for when, src, txt in items: print(f"  {when:<11} [{src}] {txt}")

# ---------------------------------------------------------------- HTML
if os.environ.get("HTML"):
    from html import escape as E
    srcs_used = [s for s in sources if any(r.get("cnt", {}).get(s) for r in rows)]
    maxh = max([r.get("active", 0) for r in rows] + [3600]) / 3600
    h = []
    h.append(f"""<!doctype html><html lang="fi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Työaika-arvio {E(os.environ['FROM'])} – {E(os.environ['TO'])}</title>
<style>
:root{{--bg:#fafaf9;--fg:#1c1917;--mut:#78716c;--line:#e7e5e4;--card:#fff;--acc:#2563eb;--acc2:#bfdbfe;--wk:#f5f5f4}}
@media (prefers-color-scheme:dark){{:root{{--bg:#1c1917;--fg:#f5f5f4;--mut:#a8a29e;--line:#44403c;--card:#292524;--acc:#60a5fa;--acc2:#1e3a8a;--wk:#231f1d}}}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}}
main{{max-width:1100px;margin:0 auto;padding:24px 16px}}h1{{font-size:22px;margin:0 0 4px}}h2{{font-size:17px;margin:32px 0 10px}}
.mut{{color:var(--mut)}}.cards{{display:flex;gap:12px;flex-wrap:wrap;margin:16px 0}}
.card{{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 16px;min-width:150px}}
.card b{{display:block;font-size:24px}}table{{border-collapse:collapse;width:100%;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}}
th,td{{padding:6px 10px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}}th{{font-weight:600;color:var(--mut);font-size:12px}}
td:first-child,th:first-child{{text-align:left}}tr.we td{{background:var(--wk)}}.bar{{position:relative;height:10px;width:160px;background:var(--line);border-radius:5px}}
.bar i{{position:absolute;left:0;top:0;bottom:0;border-radius:5px}}.bar .s{{background:var(--acc2)}}.bar .a{{background:var(--acc)}}
.wrap{{overflow-x:auto}}details{{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:10px 0;padding:0 14px}}
summary{{cursor:pointer;padding:10px 0;font-weight:600}}summary span{{font-weight:400}}
.strip{{position:relative;height:18px;background:var(--line);border-radius:4px;margin:4px 0 12px}}
.strip i{{position:absolute;top:0;bottom:0;background:var(--acc);border-radius:2px;min-width:2px;outline:1px solid var(--card)}}.strip i.m{{background:var(--acc2)}}
.ticks{{display:flex;justify-content:space-between;font-size:11px;color:var(--mut)}}
.sess{{margin:10px 0 4px;font-weight:600;font-size:13px}}ul{{list-style:none;padding:0;margin:0 0 12px}}li{{display:grid;grid-template-columns:90px 70px 1fr;gap:8px;padding:2px 0;border-bottom:1px dashed var(--line)}}
li code{{font:12px ui-monospace,monospace;color:var(--mut)}}.tag{{font-size:11px;padding:1px 6px;border-radius:9px;background:var(--line);justify-self:start}}
h3{{font-size:14px;margin:14px 0 6px}}
@media (max-width:600px){{li{{grid-template-columns:70px 1fr}}li .tag{{display:none}}}}
</style></head><body><main>
<h1>Työaika-arvio {E(os.environ['FROM'])} – {E(os.environ['TO'])}</h1>
<div class="mut">Luotu {datetime.now().strftime('%Y-%m-%d %H:%M')}. Laskettu = kellon {SLOT//60} min jaksot, joissa on yksikin tapahtuma ({SLOT//60} min / jakso). Kiinteät = käsin tiedossa olevat merkinnät; kellonajalliset yhdistetään laskettuihin jaksoihin (päällekkäisyyttä ei lasketa kahdesti), pelkät tuntimäärät lisätään päälle. Span = päivän ensimmäinen–viimeinen tapahtuma.</div>
<div class="cards"><div class="card"><span class="mut">Arvio yhteensä</span><b>{hm(tot_a)} h</b></div>
<div class="card"><span class="mut">Laskettu</span><b>{hm(tot_c)} h</b></div>
<div class="card"><span class="mut">Kiinteät lisäys</span><b>+{hm(tot_f)} h</b></div>
<div class="card"><span class="mut">Työpäiviä</span><b>{sum(1 for r in rows if r['n'])}</b></div></div>
<h2>Päivät</h2><div class="wrap"><table><tr><th>Päivä</th><th>Alku</th><th>Loppu</th><th>Span</th><th>Laskettu</th><th>Kiinteät</th><th>Arvio</th><th></th>""")
    h.append("".join(f"<th>{E(s)}</th>" for s in srcs_used) + "</tr>")
    for r in rows:
        we = ' class="we"' if r["day"].weekday() >= 5 else ""
        lab = f"{r['day']} {wd[r['day'].weekday()]}"
        if not r["n"]:
            h.append(f"<tr{we}><td>{lab}</td>" + "<td>–</td>" * (7 + len(srcs_used)) + "</tr>"); continue
        bar = (f'<div class="bar"><i class="s" style="width:{r["active"]/3600/maxh*100:.1f}%"></i>'
               f'<i class="a" style="width:{r["comp"]/3600/maxh*100:.1f}%"></i></div>')
        h.append(f'<tr{we}><td><a href="#d{r["day"]}">{lab}</a></td><td>{fmt(r["first"])}</td><td>{fmt(r["last"])}</td>'
                 f'<td>{hm(r["span"])}</td><td>{hm(r["comp"])}</td><td>{("+" + hm(r["fixed"])) if r["fixed"] else ""}</td><td><b>{hm(r["active"])}</b></td><td>{bar}</td>'
                 + "".join(f"<td>{r['cnt'].get(s, 0) or ''}</td>" for s in srcs_used) + "</tr>")
    h.append(f'<tr><td><b>Yhteensä</b></td><td></td><td></td><td>{hm(tot_s)}</td><td>{hm(tot_c)}</td><td>+{hm(tot_f)}</td><td><b>{hm(tot_a)}</b></td><td></td>'
             + "<td></td>" * len(srcs_used) + "</tr></table></div>")
    h.append("<h2>Päiväkohtaiset tiedot</h2>")
    for r in rows:
        if not r["n"]: continue
        d = r["day"]; m0 = midnight(d)
        h.append(f'<details id="d{d}" open><summary>{d} {wd[d.weekday()]} <span class="mut">· arvio {hm(r["active"])} · '
                 f'laskettu {hm(r["comp"])}{" + kiinteät " + hm(r["fixed"]) if r["fixed"] else ""} · {fmt(r["first"])}–{fmt(r["last"])}</span></summary>')
        nl, off = net_summary(d)
        if nl: h.append(f'<div class="mut" style="margin:4px 0">Verkko{" (toimisto)" if off else ""}: ' + E("; ".join(nl)) + "</div>")
        h.append('<div class="strip">' + "".join(
            f'<i class="m" title="kiinteä" style="left:{k*SLOT/864:.2f}%;width:{SLOT/864:.2f}%"></i>' for k in sorted(r["mslots"] - r["slots"]))
            + "".join(f'<i title="{fmt(m0+k*SLOT)}–{fmt(m0+(k+1)*SLOT)}" style="left:{k*SLOT/864:.2f}%;width:{SLOT/864:.2f}%"></i>' for k in sorted(r["slots"]))
            + '</div><div class="ticks">' + "".join(f"<span>{x:02d}</span>" for x in range(0, 25, 3)) + "</div>")
        if done.get(d) and nmax > 0:
            h.append("<h3>Tehty</h3><ul>" + "".join(
                f'<li><code>{tlab(t, s, txt)}</code><span class="tag">{E(s)}</span><span>{E(txt)}</span></li>' for t, s, txt in done[d])
                + "</ul>")
        h.append("<h3>Aikajana</h3>")
        for a, b, items in tl[d]:
            h.append((f'<div class="sess">Kiinteät merkinnät</div><ul>' if a is None else f'<div class="sess">{fmt(a)}–{fmt(b)}</div><ul>') + "".join(
                f'<li><code>{E(w)}</code><span class="tag">{E(s)}</span><span>{E(txt)}</span></li>' for w, s, txt in items) + "</ul>")
        h.append("</details>")
    h.append("</main></body></html>")
    open(os.environ["HTML"], "w").write("\n".join(h))
    print(f"HTML-raportti: {os.environ['HTML']}")
print(f"\nLask. = {SLOT//60} min kellojaksot joissa aktiivisuutta; Kiint. = kiinteiden merkintöjen lisäys (kellonajalliset yhdistetty laskettuihin, tuntimäärät päälle).")
if os.environ["CSV"]:
    with open(os.environ["CSV"], "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["date", "first", "last", "span_h", "computed_h", "fixed_h", "estimate_h"] + sources)
        for r in rows:
            if not r["n"]: w.writerow([r["day"]] + [""] * 6 + [0] * len(sources)); continue
            w.writerow([r["day"], fmt(r["first"]), fmt(r["last"]), round(r["span"] / 3600, 2), r["comp"] / 3600,
                        r["fixed"] / 3600, r["active"] / 3600] + [r["cnt"].get(s, 0) for s in sources])
    print(f"CSV: {os.environ['CSV']}")
PY
