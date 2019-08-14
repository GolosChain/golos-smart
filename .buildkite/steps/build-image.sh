#!/bin/bash
set -euo pipefail

IMAGETAG=${BUILDKITE_BRANCH:-master}
BRANCHNAME=${BUILDKITE_BRANCH:-master}

TAGREF=${BUILDKITE_TAG:+"tags/$BUILDKITE_TAG"}
REF=${TAGREF:-"heads/$BUILDKITE_BRANCH"}

if [[ "${IMAGETAG}" == "master" ]]; then
    BUILDTYPE="stable"
elif [[ "${IMAGETAG}" == "alfa" ]]; then
    BUILDTYPE="alfa"
else
    BUILDTYPE="latest"
fi

cd Docker
docker build -t cyberway/golos.contracts:${IMAGETAG} --build-arg branch=${BRANCHNAME} --build-arg buildtype=${BUILDTYPE} --build-arg ref=${REF} .
