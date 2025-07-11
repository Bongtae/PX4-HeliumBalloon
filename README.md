# PX4 Drone Autopilot for HeliumBalloon project

This repository is made for PX4-HeliumBalloon project.

This repository holds the [PX4](http://px4.io) flight control solution for drones, with the main applications located in the [src/modules](https://github.com/PX4/PX4-Autopilot/tree/main/src/modules) directory. It also contains the PX4 Drone Middleware Platform, which provides drivers and middleware to run drones.

For convenience, PX4-gazebo-models repository has been removed from submodule of this project, and uploaded static version of that repository.

## Getting Started

### Prerequisites

You need the following development environment:

**Ubuntu 24.04.2 LTS(noble)**

This project is based on following environment:

**PX4-AutoPilot release/1.15**

**Gazebo Simulator ("Harmonic")**

### Installing

1. Download PX4 Source code:
```
git clone https://github.com/Bongtae/PX4-HeliumBalloon.git --recursive
```

2. Run **ubuntu.sh**
```
bash ./PX4-HeliumBalloon/Tools/setup/ubuntu.sh
```

3. Install **VSCode** and **QGroundControl**

For more information, follow the link:
[PX4 Ubuntu Development Environment Setup](https://docs.px4.io/main/en/dev_setup/dev_env_linux_ubuntu.html)

## Run SITL Simulation

Build px4_sitl_default under **PX4-HeliumBalloon** folder.

```
make px4_sitl_default
```

Run gz_x500 multicopter model for test.

```
PX4_SYS_AUTOSTART=4001 PX4_SIM_MODEL=x500 ./build/px4_sitl_default/bin/px4
```

## Troubleshooting

If you changed airframe config file under init.d-posix/airframe, you should reset repository by **make distclean** and **submodule update**.

```
make distclean && git submodule update --init --recursive
```

## Maintenancer

* **Yun Taewoong** - [Bongtae](https://github.com/Bongtae)
