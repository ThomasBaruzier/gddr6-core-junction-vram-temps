# GPU temperature monitor for NVIDIA GPUs on Linux

`gputemps` displays GPU core, junction, and VRAM temperatures for compatible NVIDIA GPUs with GDDR6, GDDR6X, or GDDR7 memory.

![gputemps temperature table](https://github.com/user-attachments/assets/f92c9e98-07cc-4bc9-964d-ce616cfbc28c)

> [!WARNING]
> This project is experimental and reads undocumented GPU temperature sensors. It is provided as-is without warranty.

## Quick start

Install the required development packages, build the program, and run it as root:

```sh
sudo apt install gcc make libpci-dev nvidia-cuda-toolkit
git clone https://github.com/ThomasBaruzier/gddr6-core-junction-vram-temps
cd gddr6-core-junction-vram-temps
make
sudo ./gputemps
```

Press any key or `Ctrl+C` to exit.

## Usage

Run the live temperature table:

```sh
sudo ./gputemps
```

Display one reading and exit:

```sh
sudo ./gputemps --once
```

Output JSON Lines:

```sh
sudo ./gputemps --json
```

Monitor one or more GPUs:

```sh
sudo ./gputemps --device 0
sudo ./gputemps --device 0,2
```

Devices can be selected by NVML index, UUID, or PCI BDF.

When `--device` is not specified, `CUDA_VISIBLE_DEVICES` is used as a device filter if set. Otherwise, `NVIDIA_VISIBLE_DEVICES` is used.

Change the refresh interval:

```sh
sudo ./gputemps --refresh-ms 100
```

The minimum refresh interval is 50 milliseconds. The default is 1000 milliseconds.

### Options

- `--device <list>`: Monitor selected devices by index, UUID, or PCI BDF.
- `--json`: Output one JSON object per line.
- `--once`: Output one reading and exit.
- `--refresh-ms <ms>`: Set the refresh interval in milliseconds.
- `--help`: Show the help message.

## JSON output

Each object contains a timestamp and the selected GPU readings:

```json
{
  "timestamp": 1678886400000,
  "gpus": [{ "index": 0, "core": 55, "junction": 68, "vram": 72 }]
}
```

- `timestamp`: Unix timestamp in milliseconds.
- `index`: NVML device index.
- `core`: GPU core temperature in Celsius.
- `junction`: GPU junction temperature in Celsius.
- `vram`: VRAM temperature in Celsius.

Unavailable readings are returned as `null`.

## Building

The build requires:

- a C11 compiler
- GNU Make
- libpci development files
- NVIDIA NVML headers and library

Build with:

```sh
make
```

If `nvml.h` is outside the default include path:

```sh
make CPPFLAGS="-I$CUDA_HOME/targets/x86_64-linux/include"
```

Remove generated files:

```sh
make clean
```

Install or uninstall the program:

```sh
sudo make install
sudo make uninstall
```

## Docker build

Build the executable in Docker and copy it to the repository directory:

```sh
./build-docker.sh
sudo ./gputemps
```

## Blackwell support

Experimental junction and GDDR7 VRAM temperature support is included for RTX 5090, RTX 5080, RTX 5070 Ti, RTX 5070, and RTX 5060 Ti.

On supported Blackwell GPUs:

- `JUNC` reports the hottest valid die thermal channel or hardware hotspot reading.
- `VRAM` reports the hottest valid GDDR7 temperature source.

The same values are available through the JSON `junction` and `vram` properties.

There is no support (yet) for RTX 5060. Contributions are welcomed.

## Troubleshooting

### Junction or VRAM shows `N/A`

The program reads memory-mapped GPU temperature sensors through `/dev/mem`. On many systems, this requires adding `iomem=relaxed` to the kernel command line.

On systems using GRUB, edit:

```sh
sudo nano /etc/default/grub
```

Add `iomem=relaxed`, for example:

```text
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash iomem=relaxed"
```

Apply the change and reboot:

```sh
sudo update-grub
sudo reboot
```

Secure Boot or kernel lockdown may also prevent access to `/dev/mem`. Check Secure Boot with:

```sh
sudo mokutil --sb
```

> [!CAUTION]
> `iomem=relaxed` allows privileged processes broader access to device memory. Consider the security implications before enabling it, especially on shared systems.

### Permission denied

Run the program as root:

```sh
sudo ./gputemps
```

### NVML headers are not found

Install the NVIDIA CUDA toolkit or provide the NVML include directory when building:

```sh
make CPPFLAGS="-I$CUDA_HOME/targets/x86_64-linux/include"
```

## GPU support

### Tested and working

- RTX 3090
- RTX 4060 Ti 16GB
- RTX 4060 Max-Q
- RTX 5060 Ti

### Should work

- RTX 5090
- RTX 5080
- RTX 5070 Ti
- RTX 5070
- RTX 4090
- RTX 4080 Super
- RTX 4080
- RTX 4070 Ti Super
- RTX 4070 Ti
- RTX 4070 Super
- RTX 4070
- RTX 3090 Ti
- RTX 3080 Ti
- RTX 3080
- RTX 3080 LHR
- RTX A2000
- RTX A4500
- RTX A5000
- RTX A6000
- L4
- L40S
- A10

### Not working

- RTX 3070
- RTX 3070 LHR

## Credits

- [olealgoritme/gddr6](https://github.com/olealgoritme/gddr6): GDDR6/GDDR6X VRAM temperature support
- [jjziets/gddr6_temps](https://github.com/jjziets/gddr6_temps): GDDR6/GDDR6X junction temperature support
- [igor'sLAB](https://www.igorslab.de/en/blackwell-hotspot-ibhe-estimation-register-findings-download-nvidia-question/): RTX 5090 junction register findings
- [TechPowerUp forum](https://www.techpowerup.com/forums/threads/rtx-5070-discussion.338562/post-5557509): RTX 5070 GDDR7 register findings
- [sunnyyangyangyang/gddr7-temp](https://github.com/sunnyyangyangyang/gddr7-temp): RTX 5090 GDDR7 VRAM and junction support
- [biGGer](https://gist.github.com/biGGer/d8e8a8bacea338d232a65b530b1e2353): RTX 5070, 5070 Ti, 5080, and 5090 VRAM and junction support
