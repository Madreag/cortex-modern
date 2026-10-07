# Symbol lookup fixture

`symbol_fixture.exe` and its matching PDB contain only the three functions in
`symbol_fixture.cpp`. Unit tests read their symbols and source lines through
DbgHelp and exercise the symbolizer CLI. They never execute the fixture.
A separate negative test changes the PE's CodeView GUID and requires rejection.

Rebuild with the x64 MSVC compiler and linker, mapping the source directory to a
neutral root so no machine's paths are embedded:

```
cl /nologo /c /Z7 /Od /GS- /experimental:deterministic /d1trimfile:<dir>\ /pathmap:<dir>=C:\fixture /Fosymbol_fixture.obj symbol_fixture.cpp
link /nologo /DEBUG:FULL /ENTRY:main /SUBSYSTEM:CONSOLE /NODEFAULTLIB /OPT:NOREF /INCREMENTAL:NO /Brepro /PDBALTPATH:symbol_fixture.pdb /pathmap:<dir>=C:\fixture /OUT:symbol_fixture.exe /PDB:symbol_fixture.pdb symbol_fixture.obj
```

The linker still records its own working directory in the PDB; in the copy here
those records were rewritten in place to the same-length neutral path
`C:\fixture\symbol-fixture-build-location`, which leaves every record's size and the
PDB's identity unchanged. Inspect the ASCII and UTF-16 strings of both outputs
before committing them: only `C:\fixture` and the compiler's install paths may appear.

The controlled fixture keeps unit tests independent of whichever engine EXE
and PDB happen to be in the checkout. Actual crash symbolization still requires
the exact PDB identity embedded in that engine's PE file; no identity check is
disabled or relaxed.
