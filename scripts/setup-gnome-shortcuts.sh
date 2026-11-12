#!/bin/bash
# Configure GNOME custom keybindings for Barrier screen switching

SCRIPT_DIR="$( cd -P "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
CTL_BIN="$SCRIPT_DIR/barrier-ctl.sh"
if [ ! -x "$CTL_BIN" ] && [ -x "$HOME/.local/bin/barrier-ctl" ]; then
    CTL_BIN="$HOME/.local/bin/barrier-ctl"
fi

BINDINGS=(
    "custom0:Barrier Switch 1 (syncmaster):$CTL_BIN 1:<Primary><Shift>1"
    "custom1:Barrier Switch 2 (wesley-Inspiron):$CTL_BIN 2:<Primary><Shift>2"
    "custom2:Barrier Switch 3 (MacBook-Pro):$CTL_BIN 3:<Primary><Shift>3"
    "custom3:Barrier Switch 4 (ITS-NB0140):$CTL_BIN 4:<Primary><Shift>4"
    "custom4:Barrier Switch 5 (w1934):$CTL_BIN 5:<Primary><Shift>5"
    "custom5:Barrier Switch 6 (dmais):$CTL_BIN 6:<Primary><Shift>6"
    "custom6:Barrier Switch 0 (ITS-NB0140):$CTL_BIN 0:<Primary><Shift>0"
)

PATHS=()
for item in "${BINDINGS[@]}"; do
    IFS=":" read -r id name cmd binding <<< "$item"
    path="/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/$id/"
    PATHS+=("'$path'")
    gsettings set "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:$path" name "$name"
    gsettings set "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:$path" command "$cmd"
    gsettings set "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:$path" binding "$binding"
done

ARRAY_STR="[$(IFS=, ; echo "${PATHS[*]}")]"
gsettings set org.gnome.settings-daemon.plugins.media-keys custom-keybindings "$ARRAY_STR"
