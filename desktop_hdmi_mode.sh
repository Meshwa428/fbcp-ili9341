#!/bin/bash
"/opt/retropie/supplementary/runcommand/runcommand.sh" 0 _PORT_ "lxde" ""

# Enable touch driver
if [ -f "/usr/local/bin/touch_mode" ]; then
    sudo /usr/local/bin/touch_mode on
fi
