#!/bin/sh

set -eu

cd "$(dirname "$0")"

image=gputemps-builder
container=

cleanup()
{
    if [ -n "$container" ]; then
        docker rm -f "$container" >/dev/null 2>&1 || true
    fi
}

trap cleanup EXIT HUP INT TERM

docker build -t "$image" .
container=$(docker create "$image")
docker cp "$container:/app/gputemps" ./gputemps
chmod +x ./gputemps

cleanup
container=
trap - EXIT HUP INT TERM

printf '\nBuild done. Run with: sudo ./gputemps\n\n'
