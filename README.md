bbr-cuda
========

This is a BBR CUDA miner, based off of CPUMiner.

#### Table of Contents

* [Features](#features)
* [Dependencies](#deps)
* [Building](#building)
* [Downloads](#downloads)
* [Usage](#usage)
* [Donations](#donations)

Features
========
* Supports only Wild Keccak
* Supports only Stratum, no HTTP
* Supports as many GPUs as the driver does
* Fast - 780kh/s or more from a 750Ti at stock clocks

Dependencies
============
* libcurl
* jansson - NOT in-tree, you MUST install it

Building
========
* Unix makefile - no autotools
* The NVCC compiler driver MUST be in your PATH
* Default builds for Maxwell, use "make kepler" to build for compute 3.5

Downloads
=========
* None yet, but check back

Usage
=====
* Use -t option to set number of GPUs to mine on
* --launch-config/-l allows specifying thread blocks and threads

Donations
=========
Donations for this miner are accepted at these addresses:
* BTC: `1WoLFumNUvjCgaCyjFzvFrbGfDddYrKNR`
* BBR: `@wolf`
