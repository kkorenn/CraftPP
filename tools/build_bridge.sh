#!/bin/sh
# Builds the plugin bridge (java/: paper-api + CraftPP's Bukkit server implementation) into
# build/java/libs/craftpp-bridge.jar. Needs Gradle and a Java 25 JDK; gradle fetches paper-api 26.3.
set -e
cd "$(dirname "$0")/../java"
gradle -q jar
