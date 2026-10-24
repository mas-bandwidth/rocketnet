# tests/pcapng/regress

Minimized fuzz regressions, one `<name>.pcapng` per finding, in the corpus format of `../README.md`. The functional replay (`test_pcapng_corpus_replay`, `functional/corpus_consistency.cpp`, run by `./bin/functional`) reads every file here and requires each packet to end read or refused under a name; their tallies are not pinned. No files yet.
