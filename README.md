# GPU temperature monitor for NVIDIA GPUs on Linux

`gputemps` displays core, junction, and VRAM temperatures for compatible NVIDIA GPUs with GDDR6, GDDR6X, or GDDR7 memory.

![gputemps temperature table](https://github.com/user-attachments/assets/f92c9e98-07cc-4bc9-964d-ce616cfbc28c)

> [!WARNING]
> This project is experimental and uses undocumented GPU temperature sensors. It is provided as-is without warranty.

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

Devices can be selected by NVML index, UUID, or PCI BDF. When `--device` is not specified, `NVIDIA_VISIBLE_DEVICES` is respected.

Change the refresh interval:

```sh
sudo ./gputemps --refresh-ms 100
```

The minimum refresh interval is 50 milliseconds and the default is 1000 milliseconds.

### Options

- `--device <list>`: Monitor selected devices by index, UUID, or PCI BDF.
- `--json`: Output one JSON object per line.
- `--once`: Output one reading and exit.
- `--refresh-ms <ms>`: Set the refresh interval in milliseconds.
- `--help`: Show the help message.

## JSON output

Each object contains a timestamp and the selected GPU readings:

```json
{"timestamp":1678886400000,"gpus":[{"index":0,"core":55,"junction":68,"vram":72}]}
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

Remove generated files with:

```sh
make clean
```

Install or uninstall the program with:

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

## GDDR7 support

RTX 5090 GDDR7 VRAM temperature monitoring is supported experimentally. The `VRAM` column and JSON `vram` property show the hottest available memory temperature.

Other Blackwell GPUs currently provide core and junction temperatures only. Their VRAM temperature is shown as `N/A` or `null` until support is added.

## Troubleshooting

### Junction or VRAM shows `N/A`

The program needs access to GPU memory-mapped temperature sensors through `/dev/mem`. On many systems, this requires adding `iomem=relaxed` to the kernel command line.

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

### Expected to work

- RTX 5090, experimental GDDR7 support
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

### Partial support

- Blackwell GPUs other than RTX 5090: core and junction temperatures only

### Not working

- RTX 3070
- RTX 3070 LHR
- Cards not listed above

## Credits

- [jjziets/gddr6_temps](https://github.com/jjziets/gddr6_temps) for the original GDDR6 temperature work
- [olealgoritme/gddr6](https://github.com/olealgoritme/gddr6) for the GDDR6, GDDR6X, and RTX 5090 temperature work
- [sunnyyangyangyang/gddr7-temp](https://github.com/sunnyyangyangyang/gddr7-temp) for validating RTX 5090 GDDR7 temperature support
