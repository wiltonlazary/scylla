# Official toolchain for ScyllaDB

While we aim to build out-of-the-box on recent distributions, this isn't
always possible and not everyone runs a recent distribution. For this reason
a version-controlled toolchain is provided as a docker image.

## Quick start

If your workstation supports docker (without requiring sudo), you can build and
run Scylla easily without setting up the build dependencies beforehand:

    ./tools/toolchain/dbuild ./configure.py
    ./tools/toolchain/dbuild ninja build/release/scylla
    ./tools/toolchain/dbuild ./build/release/scylla --developer-mode 1

## The `dbuild` script

The script `dbuild` allows you to run any in that toolchain with
the working directory mounted:

    ./tools/toolchain/dbuild ./configure.py
    ./tools/toolchain/dbuild ninja

You can adjust the `docker run` command by adding more flags before the
command to be executed, separating the flags and the command with `--`.
This can be useful to attach more volumes (for data or ccache) and to
set environment variables:

    ./tools/toolchain/dbuild -v /var/cache/ccache:/var/cache/ccache -- ninja

The script also works from other directories, so if you have `scylla-ccm` checked
out alongside scylla, you can write


    ../scylla/tools/toolchain/dbuild ./ccm ...

You will have access to both scylla and scylla-ccm in the container.

Interactive mode is also supported: running `dbuild` with no arguments
will drop you into a shell, with all of the toolchain accessible.

## Obtaining the current toolchain

The toolchain is stored in a file called `tools/toolchain/image`. Normally,
`dbuild` will fetch the toolchain automatically. If you want to access
the toolchain explicitly, pull that image:

    docker pull $(<tools/toolchain/image)

## Building the toolchain

If you add dependencies (to `install-dependencies.sh` or
`seastar/install-dependencies.sh`) you should update the toolchain.

Run the command

    podman build --no-cache --pull -f tools/toolchain/Dockerfile .

and use the resulting image.

## Publishing an image

If you're a maintainer, you can tag the image and push it
using `podman push`. Tags follow the format
`scylladb/scylla-toolchain:fedora-29-[branch-3.0-]20181128`.

For master toolchains, the branch designation is omitted. In a branch, if
there is a need to update a toolchain, the branch designation is added to
the tag to avoid ambiguity.

To publish a new image, follow this procedure:

1. Pick a new name for the image (in `tools/toolchain/image`) and
   commit it. The commit updating install-dependencies.sh should
   include the toolchain change, for atomicity. Do not push the commit
   to `next` yet.
2. Push the commit to a personal repository/branch.
3. Perform the following on an x86 and an ARM machine:
    1. check out the branch containing the new toolchain name
    2. Run `git submodule update --init --recursive` to make sure
       all the submodules are synchronized
    3. Run `podman build --no-cache --pull --tag mytag-arch -f tools/toolchain/Dockerfile .`, where mytag-arch is a new, unique tag that is different for x86 and ARM.
    4. Push the resulting images to a personal docker repository.
4. Now, create a multiarch image with the following:
    1. Pull the two images with `podman pull`. Let's call the two tags
       `mytag-x86` and `mytag-arm`.
    2. Create the new toolchain manifest with `podman manifest create $(<tools/toolchain/image)`
    3. Add each image with `podman manifest add --all $(<tools/toolchain/image) mytag-x86` and `podman manifest add --all $(<tools/toolchain/image) mytag-arm`
    4. Push the image with `podman manifest push --all $(<tools/toolchain/image) docker://$(<tools/toolchain/image)`
5. Now push the commit that updated the toolchain with `git push`.

## Troubleshooting

When running `sudo` inside the container fails like this:
```
$ tools/toolchain/dbuild /bin/bash
bash-4.4$ sudo dnf install gdb
sudo: unknown uid 1000: who are you?
```

You can work it around by disabling SELinux on the host before running `dbuild`:
```
$ sudo setenforce 0
```
