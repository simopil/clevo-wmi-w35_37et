# Clevo W35_37ET WMI Module

It's a kernel module that enables some functions and readings of Clevo barebones equipped with a W35_37ET mainboard.
Based on clevo-wmi module by
[Lekensteyn](https://github.com/Lekensteyn/acpi-stuff/blob/master/clevo-wmi/clevo-wmi.c)

## Description and Functions

* Usable hotkeys:
  -  VGA Button as KEY_PROG1
  -  FN+ESC as KEY_PROG2
* VGA LED in VGA button:
  - There is an exposed /proc/acpi/vga_led file that accepts g|G to make led green and y|Y to make it yellow, the output of this file is the current led color readed from EC.
* FAN Speed reading
  - Because lmsensors & co. cannot read the fan speed in this mainboard, this module provides a read-only /proc/acpi/fan_rpm that reads speed directly from EC and converts it to a readable format
* Module param: init_color=0|1 (yellow|green)
## Disclaimer: calls to switch led color are WMBB based via ACPI methods, readings from EC shouldn't be dangerous.

## Example: bash script that runs every x seconds changes light based on temperature [AS ROOT]

```bash
#!/bin/bash
#green if temp < 70, else yellow, blinking critical
if (( $(echo "$(sensors | grep -oP 'Package.*?\+\K[0-9.]+') > 70" | bc -l) )); then
    echo y > /proc/acpi/vga_led
    #if temp > 90 led starts blinking green/yellow
    while (( $(echo "$(sensors | grep -oP 'Package.*?\+\K[0-9.]+') > 90" | bc -l) )); do
        echo g > /proc/acpi/vga_led
        sleep 0.3s
        echo y > /proc/acpi/vga_led
        sleep 0.3s
    done
else
    echo g > /proc/acpi/vga_led
fi

```

## Install
```bash
make
make install
```
## Load
```bash
modprobe w35_37et-wmi
```

## Contributing
I don't consider myself an expert about kernel modules, advices and pull requests are welcome. You can contact me here or at pilia.simone96@gmail.com 
