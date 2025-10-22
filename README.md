## Core, Junction, and VRAM temperature reader for Linux + GDDR6/GDDR6X GPUs

<br>

![image](https://github.com/user-attachments/assets/f92c9e98-07cc-4bc9-964d-ce616cfbc28c)

<br>

Warning: This repo is experimental and may not work as intended. The code is provided as-is without any warranty of any kind. If you have a problem, please open an issue!

<br>

## Quickstart

Assuming you have libpci and cuda, you can directly build and run the project like this:

```
curl -sO https://raw.githubusercontent.com/ThomasBaruzier/gddr6-core-junction-vram-temps/refs/heads/main/gputemps.c && gcc gputemps.c -o gputemps -O3 -lnvidia-ml -lpci -I"$CUDA_HOME/targets/x86_64-linux/include" && sudo ./gputemps
```

If you don't have the dependencies, you can use Docker for the build (will download cuda):

```
git clone https://github.com/ThomasBaruzier/gddr6-core-junction-vram-temps && cd gddr6-core-junction-vram-temps && ./build-docker.sh && sudo ./gputemps
```

Use --json to output GPU temperatures in JSON format.

Use --once to output temperatures a single time. By default, the program runs in continuous (watch) mode.

If this didn't work, please continue reading

<br>

## Dependencies

- libpci-dev 
```
sudo apt install libpci-dev
```

- cuda
```
sudo apt install nvidia-cuda-toolkit
```

<br>

## Building

```
gcc gputemps.c -o gputemps -O3 -lnvidia-ml -lpci
```

If you get the error `nvml.h: No such file or directory`, try adding `-I/path/to/cuda/targets/x86_64-linux/include`

<br>

## Executing

```
sudo ./gputemps
```

Press any key or `CTRL+C` to exit

<br>

## Troubleshooting (in case of mmap error)

- The following kernel boot parameter should be used: `iomem=relaxed`

```
sudo nano /etc/default/grub
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash iomem=relaxed"
sudo update-grub
sudo reboot
```

- Disabling Secure Boot
  
This can be done in the UEFI/BIOS configuration or using [mokutil](https://wiki.debian.org/SecureBoot#Disabling.2Fre-enabling_Secure_Boot):

```
mokutil --disable-validation
```

Check state with:
```
$ sudo mokutil --sb
SecureBoot disabled
```

<br>

## Tested and working

- RTX 3090 (GA102)
- RTX 4060 Ti 16GB (AD106)
- RTX 4060 Max-Q (AD107)

<br>

## Should be working
- RTX 4090 (AD102)
- RTX 4080 Super (AD103)
- RTX 4080 (AD103)
- RTX 4070 Ti Super (AD103)
- RTX 4070 Ti (AD104)
- RTX 4070 Super (AD104)
- RTX 4070 (AD104)
- RTX 3090 Ti (GA102)
- RTX 3080 Ti (GA102)
- RTX 3080 (GA102)
- RTX 3080 LHR (GA102)
- RTX A2000 (GA106)
- RTX A4500 (GA102)
- RTX A5000 (GA102)
- RTX A6000 (AD102)
- L4 (AD104)
- L40S (AD102)
- A10 (GA102)

<br>

## Not working
- RTX 3070 (GA104)
- RTX 3070 LHR (GA104)
- Any other card not listed above

<br>

## Credits
- https://github.com/jjziets/gddr6_temps: For showing me how to get VRAM and Junction temps
- https://github.com/olealgoritme/gddr6: For pioneering the method to access undocumented GPU registers, and this README
