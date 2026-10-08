# General packing equivalence

The Reference files are frozen test-only copies of the pre-extraction General
implementation at client commit 9724f14. They are not linked into the client and
must not be updated to match a changed implementation. They provide an exact
differential oracle for pools, workers, acquisition remainder, execution masks,
tie ordering and bounded-search reports, including missing/partial topology.
The suite also compares full RG results, including reservations, inactive demand
and partial topology. Single-resource slice execution is checked against the old
mask builder. The production packer remains the only live packing implementation.
