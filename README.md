# Http (rhymuweb)

This is a library which implements [RFC
7230](https://tools.ietf.org/html/rfc7230), "Hypertext Transfer Protocol
(HTTP/1.1): Message Syntax and Routing".

[![Crates.io](https://img.shields.io/crates/v/rhymuweb.svg)](https://crates.io/crates/rhymuweb)
[![Documentation](https://docs.rs/rhymuweb/badge.svg)][dox]

More information about the Rust implementation of this library can be found in
the [crate documentation][dox].

[dox]: https://docs.rs/rhymuweb

The purpose of this library is to provide `Request` and `Response` types which
can be used parse and generate Hypertext Transfer Protocol (HTTP) requests and
responses.

This is a multi-language library containing independent implementations
for the following programming languages:

* C++
* Rust

## Building the C++ Implementation

A portable library is builtwhich depends only on the C++11 compiler and
standard library, so it should be supported on almost any platform.  The
following are recommended toolchains for popular platforms.

* Windows -- [Visual Studio](https://www.visualstudio.com/) (Microsoft Visual
  C++)
* Linux -- clang or gcc
* MacOS -- Xcode (clang)

## Building

This library is not intended to stand alone.  It is intended to be included in
a larger solution which uses [CMake](https://cmake.org/) to generate the build
system and build applications which will link with the library.

There are two distinct steps in the build process:

1. Generation of the build system, using CMake
2. Compiling, linking, etc., using CMake-compatible toolchain

### Prerequisites

* [CMake](https://cmake.org/) version 3.8 or newer
* C++11 toolchain compatible with CMake for your development platform (e.g.
  [Visual Studio](https://www.visualstudio.com/) on Windows)
* [Timekeeping](https://github.com/rhymu8354/Timekeeping.git) - a library
  of classes and interfaces dealing with tracking time and scheduling work

### Build system generation

Generate the build system using [CMake](https://cmake.org/) from the solution
root.  For example:

```bash
mkdir build
cd build
cmake -G "Visual Studio 15 2017" -A "x64" ..
```

### Compiling, linking, et cetera

Either use [CMake](https://cmake.org/) or your toolchain's IDE to build.
For [CMake](https://cmake.org/):

```bash
cd build
cmake --build . --config Release
```
