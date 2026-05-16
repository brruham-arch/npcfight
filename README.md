# NPC Fight — GTA SA Android AML Mod

Spawn NPC dan buat mereka saling bertarung di GTA SA Android (Offline).

## Build
Push ke GitHub → Actions otomatis build → download artifact `libnpcfight.so`

## Instalasi
Letakkan `libnpcfight.so` di folder mod AML.

## Cara Pakai (via Termux)
```bash
echo "spawn 267 30" > /sdcard/npcfight.txt   # SWAT + AK47
echo "spawn 265 22" > /sdcard/npcfight.txt   # COP + Pistol
echo "spawn"        > /sdcard/npcfight.txt   # default
echo "clear"        > /sdcard/npcfight.txt   # clear list
```

## Model ID
| ID  | Model  |
|-----|--------|
| 265 | COP    |
| 267 | SWAT   |
| 287 | ARMY   |
| 102 | BALLAS |
| 105 | GROVE  |
| 114 | VAGOS  |

## Weapon ID
| ID | Weapon  |
|----|---------|
| 22 | Pistol  |
| 25 | Shotgun |
| 30 | AK47    |
| 31 | M4      |
|  4 | Knife   |
