# htstress for Windows
This is a fork of [htstress](https://github.com/arut/htstress) made as a port to Windows and Visual C, while still retaining source compatibility with Linux

# Compilation
## Windows (duh)
Just open the Visual Studio solution and hit the compile button.
## Linux
Just run ./build.sh

# Basic usage
Arguments are similar to ab's:

-n total number of requests to make [OPTIONAL]\
-c concurrency\
-t threads number

If -n is missing, endless benchmark is started.
Use Ctrl-C to abort and see the result.

Set threads number equal to the number of
cores in your CPU for best results.

# TODO (random order)
- [ ] Implement atomic numbers again in a cross-platform way.
- [ ] Error checking on command line arguments.

# Original readme
is on README.original file.