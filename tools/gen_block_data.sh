#!/bin/sh
# data/blocks.bin: block state layout + behaviour flags (tools/blockdata/BlockData.java, run once offline).
set -e
cd "$(dirname "$0")/.."
JAVA_HOME=${JAVA_HOME:-/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home}
CP="vanilla/versions/26.3/server-26.3.jar:$(find vanilla/libraries -name '*.jar' | tr '\n' ':')"
OUT=build/blockdata
mkdir -p $OUT
"$JAVA_HOME/bin/javac" -nowarn -d $OUT -cp "$CP" tools/blockdata/BlockData.java
(cd $OUT && "$JAVA_HOME/bin/java" -cp ".:$(echo "$CP" | sed "s#vanilla/#$PWD/../../vanilla/#g")" BlockData "$PWD/../../data/blocks.bin")
