#!/bin/bash
# Configure GNOME custom keybindings for Barrier screen switching

BINDINGS=(
    "custom0:Barrier Switch 1 (syncmaster):barrier-ctl 1:<Primary><Shift>1"
    "custom1:Barrier Switch 2 (wesley-Inspiron):barrier-ctl 2:<Primary><Shift>2"
    "custom2:Barrier Switch 3 (MacBook-Pro):barrier-ctl 3:<Primary><Shift>3"
    "custom3:Barrier Switch 4 (ITS-NB0140):barrier-ctl 4:<Primary><Shift>4"
    "custom4:Barrier Switch 5 (w1934):barrier-ctl 5:<Primary><Shift>5"
    "custom5:Barrier Switch 6 (dmais):barrier-ctl 6:<Primary><Shift>6"
    "custom6:Barrier Switch 0 (ITS-NB0140):barrier-ctl 0:<Primary><Shift>0"
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
