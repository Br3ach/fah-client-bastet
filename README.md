Folding@Home Desktop Client
===========================

Folding@home is a distributed computing project -- people from
throughout the world download and run software to band together to
make one of the largest supercomputers in the world. Every computer
takes the project closer to our goals. Folding@home uses novel
computational methods, coupled to distributed computing, to simulate
problems millions of times more challenging than previously achieved.

Protein folding is linked to disease, such as Alzheimer's, ALS,
Huntington's, Parkinson's disease, and many Cancers.

Moreover, when proteins do not fold correctly (i.e. "misfold"), there
can be serious consequences, including many well known diseases, such
as Alzheimer's, Mad Cow (BSE), CJD, ALS, Huntington's, Parkinson's
disease, and many Cancers and cancer-related syndromes.

# What is protein folding?
Proteins are biology's workhorses -- its "nanomachines." Before
proteins can carry out these important functions, they assemble
themselves, or "fold." The process of protein folding, while critical
and fundamental to virtually all of biology, in many ways remains a
mystery.

# This software
This repository contains a new [Open-Source](https://opensource.org/osd)
version of the Folding@home client software.  The complete client software
consists of a frontend and a backend.  This repository contains the backend.
The frontend is in a separate repository at
[fah-web-client-bastet](https://github.com/foldingathome/fah-web-client-bastet).
The backend can be configured to run on its own without any user interaction.
The frontend is a web application which normally will run at
https://app.foldingathome.org/ but can also be run locally for testing and
development purposes.

# Quick Start for Debian Linux

(see the [BUILDING-RPM.md](BUILDING-RPM.md) file for instructions on how to build the RPM package)

## Install the Prerequisites
```
sudo apt update
sudo apt install -y scons git npm build-essential fakeroot libssl-dev zlib1g-dev libbz2-dev liblz4-dev libsystemd-dev
```

## Get the code
```
git clone https://github.com/cauldrondevelopmentllc/cbang
git clone https://github.com/foldingathome/fah-client-bastet
git clone https://github.com/foldingathome/fah-web-client-bastet
```

## Build a specific version (Optional)
To checkout the code for a specific version of the client run:

```
git -C cbang checkout bastet-v<version>
git -C fah-client-bastet checkout v<version>
git -C fah-web-client-bastet checkout v<version>
```

Where ``<version>`` is a version number.

Run ``git -C fah-client-bastet tag`` to list available version numbers.

## Build the Folding@home Client
```
export CBANG_HOME=$PWD/cbang
scons -C cbang
scons -C fah-client-bastet
scons -C fah-client-bastet package
```

## Install the package
The last build step builds the Debian package.  You can then install it like this:

```
sudo apt install ./fah-client-bastet/fah-client_<version>_amd64.deb
```

Where ``<version>`` is the software version number.

Folding@home Client older than v8 will be automatically removed.

## Folding@home Client Service
After installation, the service runs and will automatically restart on startup.

**File storage locations:**
- Logs: `/var/log/fah-client`
- Data: `/var/lib/fah-client`

Related service commands for **Status, Start, Stop, Restart:**
```
systemctl status --no-pager -l fah-client
sudo systemctl start fah-client
sudo systemctl stop fah-client
sudo systemctl restart fah-client
```

NOTE: If the Folding@home Client is not being run as a service and is manually
run with `fah-client` in a terminal window, the data and log folders will be
created in the working directory where it is run from.

## Start the development web server
Use these commands to run your own frontend server for testing purposes.  In
production, this code will run at https://app.foldingathome.org/.

```
cd fah-web-client-bastet
npm i
npm run dev
```

With the development server running, open http://localhost:5173/ in a browser to
view the client frontend.

## CPU-affinity v6 dependencies

CPU-affinity v6 CI pins the topology API in official cbang revision
`8acdbdf374fa1a8bc1e7ebeed2e98770617030e3`:
https://github.com/CauldronDevelopmentLLC/cbang/commit/8acdbdf374fa1a8bc1e7ebeed2e98770617030e3
For reproducible v6 builds, check out this revision before building cbang:

```
git -C cbang checkout 8acdbdf374fa1a8bc1e7ebeed2e98770617030e3
```

Rebuild cbang and the client together. The macOS CI job applies a targeted
count-conversion workaround to this pin; consult `.github/workflows/ci.yml`
when reproducing that build. FAH determines strict affinity support on
Windows/Linux. macOS retains legacy scheduling.


## CPU allocation limits and rollback

General-only saved settings retain legacy oversubscription validation when no
GPU CPU reservations are configured. Homogeneous managed allocation reduces
runtime workers under shortage; hybrid/unknown General-only configurations retain
legacy OS scheduling. Saved requests remain unchanged. Linux's single clustered
performance level does not prove homogeneous hardware, so it retains legacy
General scheduling until cbang exposes stronger classification information.
CPU allocation edits in performance-class mode validate total and per-class
capacity. Runtime class shortages reduce workers within the selected levels;
General overcommit cannot move class workers to another level. Unknown class
topology suspends class work instead of launching it as General. Active GPU CPU
reservations also enforce the remaining capacities,
even when all CPU groups use General mode.

On Windows systems with multiple processor groups, managed affinity and exclusive
GPU CPU reservations are unavailable. General CPU folding uses legacy OS scheduling.

When CPU resources are insufficient, allocation ties use resource-group names
in lexical order. Earlier names can consistently receive the indivisible remainder.
Priority does not rotate over time, and whole-core allocation can leave a later
resource group waiting when no complete core remains.

Exclusive GPU reservations follow lexical resource-group name order, then lexical GPU
ID order within each group. During a shortage, earlier successful reservations
keep their cores. A later GPU waits if its full reservation cannot be satisfied,
without falling back to shared cores. This priority does not rotate over time.

While a resource group is eligible to fold, each enabled usable GPU keeps its
exclusive CPU reservation even without an assigned or running GPU job. Assignment
retries and preparation delays can therefore leave those cores idle. This keeps
placement stable and avoids disrupting CPU jobs whenever GPU work arrives.
Pausing the group or entering a group-level wait releases its planned reservation;
any stopping FahCore retains live ownership until it exits.

A stock client can load these settings and retain the general CPU totals. It
removes unsupported class, GPU reservation and GPU priority settings when saving.
Returning to v6 therefore requires selecting those preferences again.

See [ARCHITECTURE.md](ARCHITECTURE.md#cpu-policy-runtime-ownership-and-live-affinity)
for allocation and launch contracts, and [TESTING.md](TESTING.md) for tester and
developer validation instructions.
