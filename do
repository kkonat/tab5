#!/usr/bin/env bash
#
# Run one of the scripts in scripts/ with the interpreter it needs.
#
# Everything under scripts/ needs the ESP-IDF virtualenv's python rather than
# whatever "python" happens to be on PATH - the system one has no pyserial, and
# the failure ("pyserial is missing") arrives one step after the mistake, which
# is the wrong end to debug it from. This finds the right interpreter and gets
# out of the way:
#
#     ./do screencap                  # the port is auto-detected
#     ./do upload --app hello
#     ./do                            # what there is to run
#
# Anything after the script name is passed through untouched, so the scripts'
# own --help still works: ./do screencap --help
#
# Which python, in order: $NEOS_PYTHON, then the environment idf.py exports,
# then the newest venv under $IDF_TOOLS_PATH, then a python on PATH that can
# import serial. Any of those can be set in .env.local, which is read first -
# see .env.local.example, and the "Setting local paths" section of the README.
#
# It has to be run as ./do, with the path: "do" is a shell keyword, so a bare
# "do screencap" is a syntax error before the shell gets as far as looking for
# a command by that name. Same in PowerShell, where the wrapper is do.ps1.
#
set -u

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dir="$repo/scripts"

# Machine-local settings: where the IDF lives, which port the tablet is on,
# which letter the card reader took. The file is gitignored and .env.local.example
# documents the keys. Read rather than sourced, so a stray line cannot run as a
# command, and only for keys that are not already set, so a shell that has been
# through IDF's export script keeps the values it put there.
CR=$'\r'
env_local() {
    local file="$repo/.env.local" line key value q
    [ -f "$file" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$CR}"
        line="${line#"${line%%[![:space:]]*}"}"
        case "$line" in ''|'#'*) continue ;; 'export '*) line="${line#export }" ;; esac
        case "$line" in *=*) ;; *) continue ;; esac
        key="${line%%=*}"; value="${line#*=}"
        key="${key//[[:space:]]/}"
        value="${value#"${value%%[![:space:]]*}"}"
        value="${value%"${value##*[![:space:]]}"}"
        q="${value:0:1}"
        if [ ${#value} -ge 2 ] && [ "$q" = "${value: -1}" ] && { [ "$q" = '"' ] || [ "$q" = "'" ]; }; then
            value="${value:1:${#value}-2}"
        fi
        [ -n "${!key:-}" ] || printf -v "$key" '%s' "$value"
        export "${key?}"
    done < "$file"
}
env_local

find_python() {
    if [ -n "${NEOS_PYTHON:-}" ] && [ -x "$NEOS_PYTHON" ]; then
        echo "$NEOS_PYTHON"
        return 0
    fi

    # Set by the IDF export, so a shell that has already been through that
    # keeps using the same interpreter the build does.
    if [ -n "${IDF_PYTHON_ENV_PATH:-}" ]; then
        for py in "$IDF_PYTHON_ENV_PATH/Scripts/python.exe" "$IDF_PYTHON_ENV_PATH/bin/python"; do
            [ -x "$py" ] && { echo "$py"; return 0; }
        done
    fi

    # Where the IDF installer puts its toolchains and venvs. Per-user by
    # default, so the fallback is that default rather than any one install;
    # IDF_TOOLS_PATH in the environment or .env.local overrides it.
    local tools="${IDF_TOOLS_PATH:-$HOME/.espressif}"
    local py
    # Reverse order so idf5.4_py3.11_env wins over an older idf5.2_py3.9_env
    # left behind by a previous install.
    for py in $(ls -d "$tools"/python_env/*/Scripts/python.exe "$tools"/python_env/*/bin/python 2>/dev/null | sort -r); do
        [ -x "$py" ] && { echo "$py"; return 0; }
    done

    # A plain python will do if somebody has installed pyserial into it.
    for py in python3 python; do
        if command -v "$py" >/dev/null 2>&1 && "$py" -c 'import serial' >/dev/null 2>&1; then
            command -v "$py"
            return 0
        fi
    done

    echo "no ESP-IDF python found - looked under $tools/python_env." >&2
    echo "Set IDF_TOOLS_PATH (if the IDF is installed elsewhere) or NEOS_PYTHON" >&2
    echo "(the interpreter to use) in .env.local, or install pyserial into the" >&2
    echo "python on PATH. See .env.local.example." >&2
    return 1
}

# The first line of a script's docstring or its .SYNOPSIS, for the listing.
summary() {
    case "$1" in
        *.py)  awk 'f && NF { sub(/^[ \t]+/, ""); print; exit } /^[ \t]*"""/ { f = 1 }' "$1" ;;
        *.ps1) awk 'f && NF { sub(/^[ \t]*#?[ \t]*/, ""); print; exit } /\.SYNOPSIS/ { f = 1 }' "$1" ;;
        *)     awk 'NR > 1 && /^#[^!]/ { sub(/^#[ \t]*/, ""); if (NF) { print; exit } }' "$1" ;;
    esac
}

usage() {
    echo "usage: ./do <script> [arguments]"
    echo
    # By name, not by file: restore-flash exists as both a .sh and a .ps1 and
    # is still one thing to run.
    # _env.py and env.ps1 are imported by the others, not run: a name that
    # starts with _, and the settings helper itself, are not commands.
    local name path
    for name in $(ls "$dir" | sed -n 's/\.\(py\|sh\|ps1\)$//p' | grep -v '^_' | grep -vx 'env' | sort -u); do
        for path in "$dir/$name".py "$dir/$name".sh "$dir/$name".ps1; do
            [ -f "$path" ] || continue
            printf '  %-15s %.57s\n' "$name" "$(summary "$path")"
            break
        done
    done
    echo
    echo "  Arguments after the name go to the script: ./do screencap --help"
}

if [ $# -eq 0 ]; then
    usage
    exit 0
fi

name="$1"
shift

# .py first, then the shell's own. A name given with its extension is taken as
# typed, so `./do deploy-card.ps1` still reaches the file it names.
script=""
for candidate in "$name.py" "$name.sh" "$name.ps1" "$name"; do
    if [ -f "$dir/$candidate" ]; then
        script="$dir/$candidate"
        break
    fi
done

if [ -z "$script" ]; then
    echo "no scripts/$name.py, scripts/$name.sh or scripts/$name.ps1" >&2
    echo >&2
    usage >&2
    exit 1
fi

case "$script" in
    *.py)
        py="$(find_python)" || exit 1
        exec "$py" "$script" "$@"
        ;;
    *.sh)
        exec bash "$script" "$@"
        ;;
    *.ps1)
        # Only reachable on Windows, which is where the .ps1-only scripts are.
        if ! command -v powershell.exe >/dev/null 2>&1; then
            echo "$script needs PowerShell, which is not on PATH" >&2
            exit 1
        fi
        exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$script" "$@"
        ;;
    *)
        echo "do not know how to run $script" >&2
        exit 1
        ;;
esac
