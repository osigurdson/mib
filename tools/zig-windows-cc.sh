#!/bin/sh
exec "${ZIG:-zig}" cc -target x86_64-windows-gnu "$@"
