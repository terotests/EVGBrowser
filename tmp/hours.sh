#!/usr/bin/env bash
# hours.sh - arvioi työtunnit per päivä koneen aktiivisuusjäljistä.
#
# Lähteet (kaikki valinnaisia; jos lähde puuttuu tai epäonnistuu, se ohitetaan):
#   login     last/wtmp: kirjautumiset ja uloskirjautumiset
#   journal   journalctl: boot, suspend/resume, lukitus/avaus, logind-sessiot
#   jenkins   Jenkins REST API: buildien aloitus- ja loppuajat
#   sliqtly   Sliqtly (localhost:8080): dokumenttien created/updated-aikaleimat
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
#   SOURCES="git jenkins" ./hours.sh
#
# Asetukset ympäristömuuttujilla (oletukset alla):
#   FROM, TO                 päivämäärävälin päät (YYYY-MM-DD, TO mukaan lukien)
#   SOURCES                  "login journal jenkins sliqtly git shell files"
#   GAP_MIN=30  PAD_MIN=10   jaksotus
#   MIN_EVENTS_DAY=1         päivä näytetään vain jos tapahtumia vähintään näin monta
#   USER_NAME=$USER          login/journal-suodatus
#
#   JENKINS_URL              esim. http://localhost:8081 (tyhjä = ohitetaan)
#   JENKINS_USER, JENKINS_TOKEN   API-token (Jenkins > User > Configure)
#   JENKINS_BUILD_USER       vain tämän käyttäjän käynnistämät buildit (tyhjä = kaikki)
#   JENKINS_DEPTH=3          kuinka syvälle kansioita (folder/multibranch) kuljetaan
#
#   SLIQTLY_URL=http://localhost:8080
#   SLIQTLY_ENDPOINTS        API-polut joita kokeillaan järjestyksessä, ks. alla
#   SLIQTLY_TOKEN / SLIQTLY_COOKIE  autentikointi jos API vaatii
#   SLIQTLY_JSON             vaihtoehtoisesti JSON-tiedosto (esim. vienti)
#   SLIQTLY_DATA_DIR         vaihtoehtoisesti datahakemisto (tiedostojen mtime)
#
#   GIT_REPOS                välilyönnillä erotettu lista repoja
#   GIT_SCAN_DIRS=$HOME      hakemistot joista .git-hakemistot etsitään (jos GIT_REPOS tyhjä)
#   GIT_SCAN_DEPTH=4
#   GIT_AUTHOR               oletus: git config --global user.email; useita: "a@x\|b@y"
#   GIT_REFLOG=1             lisää reflog-tapahtumat (checkout, rebase, commit --amend ...)
#
#   WORK_DIRS=$HOME/src      files-lähteen hakemistot (välilyönnillä erotettu)
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

usage() { sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }
while [ $# -gt 0 ]; do
  case "$1" in
    --from) FROM="$2"; shift 2;;
    --to) TO="$2"; shift 2;;
    --gap) GAP_MIN="$2"; shift 2;;
    --pad) PAD_MIN="$2"; shift 2;;
    --sources) SOURCES="$2"; shift 2;;
    --csv) CSV="$2"; shift 2;;
    --dump) DUMP="$2"; shift 2;;     # kirjoittaa raakatapahtumat tiedostoon
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
: > "$EVENTS"

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
  curl -fsS --max-time 60 "${auth[@]}" -G "${JENKINS_URL%/}/api/json" \
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
# Oletus: Sliqtly-palvelin palauttaa dokumenttilistan JSONina, jossa jokaisella
# dokumentilla on ISO-aikaleimat kentissä created/updated (MCP-rajapinnan
# list_presentations palauttaa juuri nämä kentät). Tarkkaa REST-polkua ei tiedetä,
# joten kokeillaan useita; ensimmäinen JSON-vastaus käytetään. Jäsennin etsii
# rekursiivisesti kaikki aikaleimakentät, joten JSON-rakenteella ei ole väliä.
# Rajoitus: "updated" kertoo vain VIIMEISIMMÄN muokkauksen, ei koko historiaa,
# joten aiemmat muokkauspäivät eivät näy tästä lähteestä.
src_sliqtly() {
  local url="${SLIQTLY_URL:-http://localhost:8080}" got=""
  local eps="${SLIQTLY_ENDPOINTS:-/api/presentations?limit=1000 /api/decks /api/presentations /api/stories /api/rooms /api/files /api/documents}"
  if [ -n "${SLIQTLY_JSON:-}" ] && [ -r "$SLIQTLY_JSON" ]; then
    cp "$SLIQTLY_JSON" "$TMP/sliqtly_0.json"; got=1
  elif command -v curl >/dev/null; then
    local hdr=() i=0
    [ -n "${SLIQTLY_TOKEN:-}" ] && hdr+=(-H "Authorization: Bearer $SLIQTLY_TOKEN")
    [ -n "${SLIQTLY_COOKIE:-}" ] && hdr+=(-H "Cookie: $SLIQTLY_COOKIE")
    for ep in $eps; do
      i=$((i+1))
      if curl -fsS --max-time 20 "${hdr[@]}" -H 'Accept: application/json' "${url%/}$ep" -o "$TMP/sliqtly_$i.json" 2>/dev/null \
         && python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$TMP/sliqtly_$i.json" 2>/dev/null; then
        log sliqtly "JSON: ${url%/}$ep"; got=1
      else rm -f "$TMP/sliqtly_$i.json"; fi
    done
  fi
  if [ -n "$got" ]; then
    python3 - "$TMP"/sliqtly_*.json <<'PY' | sort -u | emit sliqtly
import json, sys, re
from datetime import datetime
KEYS = re.compile(r"(created|updated|modified|changed|saved|edited)(_?at|_?on|_?time)?$", re.I)
def ts(v):
    if isinstance(v, (int, float)):
        return int(v / 1000 if v > 1e11 else v)
    if isinstance(v, str):
        try: return int(datetime.fromisoformat(v.replace("Z", "+00:00")).timestamp())
        except ValueError: return None
def walk(o, label=""):
    if isinstance(o, dict):
        lab = str(o.get("title") or o.get("name") or o.get("deck_id") or o.get("id") or label)[:60]
        for k, v in o.items():
            if KEYS.search(k):
                t = ts(v)
                if t: print(f"{t}\t{k}: {lab}")
            else: walk(v, lab)
    elif isinstance(o, list):
        for x in o: walk(x, label)
for f in sys.argv[1:]:
    walk(json.load(open(f)))
PY
  fi
  if [ -n "${SLIQTLY_DATA_DIR:-}" ] && [ -d "$SLIQTLY_DATA_DIR" ]; then
    find "$SLIQTLY_DATA_DIR" -type f -newermt "$FROM" ! -newermt "$(date -d "$TO +1 day" +%F)" \
      -printf '%T@\t%P\n' 2>/dev/null | emit sliqtly
  fi
  [ -z "$got" ] && [ -z "${SLIQTLY_DATA_DIR:-}" ] && log sliqtly "ei JSON-vastausta osoitteesta $url (aseta SLIQTLY_ENDPOINTS, SLIQTLY_JSON tai SLIQTLY_DATA_DIR)"
  log sliqtly "$(count_src sliqtly) tapahtumaa"
}

# ------------------------------------------------------------- git
src_git() {
  command -v git >/dev/null || { log git "git puuttuu"; return; }
  local author="${GIT_AUTHOR:-$(git config --global user.email 2>/dev/null)}" repos="${GIT_REPOS:-}"
  if [ -z "$repos" ]; then
    for d in ${GIT_SCAN_DIRS:-$HOME}; do
      repos="$repos $(find "$d" -maxdepth "${GIT_SCAN_DEPTH:-4}" -name .git \( -type d -o -type f \) \
                -not -path '*/node_modules/*' -prune 2>/dev/null | sed 's|/\.git$||' | tr '\n' ' ')"
    done
  fi
  local n=0
  for r in $repos; do
    [ -e "$r/.git" ] || [ -d "$r/objects" ] || continue
    n=$((n+1)); local name; name=$(basename "$r")
    git -C "$r" log --all --no-merges ${author:+--author="$author"} \
        --since="@$START_EPOCH" --until="@$END_EPOCH" --format="%at%x09$name commit %h %s" 2>/dev/null \
      | cut -c1-120 | emit git
    if [ "${GIT_REFLOG:-1}" = 1 ]; then
      # reflog on paikallinen eli kertoo omasta toiminnasta tällä koneella
      git -C "$r" reflog --all --date=unix --format="%gd%x09%gs" 2>/dev/null \
        | sed -nE "s/^[^@]*@\{([0-9]+)\}\t(.*)$/\1\t$name reflog \2/p" | cut -c1-120 | emit git
    fi
  done
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
  for d in ${WORK_DIRS:-$HOME/src}; do
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
FROM="$FROM" TO="$TO" SOURCES="$SOURCES" VERBOSE="$VERBOSE" python3 - "$EVENTS" <<'PY'
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
    days[d].append((t, p[1]))

def hm(sec): sec = int(round(sec / 60)); return f"{sec // 60}:{sec % 60:02d}"
rows = []
d, end = date.fromisoformat(os.environ["FROM"]), date.fromisoformat(os.environ["TO"])
while d <= end:
    ev = sorted(days.get(d, []))
    if len(ev) < min_ev:
        rows.append(dict(day=d, n=0)); d += timedelta(days=1); continue
    sessions, s0, prev = [], ev[0][0], ev[0][0]
    for t, _ in ev[1:]:
        if t - prev > gap: sessions.append((s0, prev)); s0 = t
        prev = t
    sessions.append((s0, prev))
    active = sum(b - a + pad for a, b in sessions)
    cnt = defaultdict(int)
    for _, s in ev: cnt[s] += 1
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
