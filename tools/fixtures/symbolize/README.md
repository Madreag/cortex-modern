# Symbol lookup fixture

`symbol_fixture.exe` and its matching PDB contain only the three functions in
`symbol_fixture.cpp`. Unit tests read their symbols and source lines through
DbgHelp and exercise the symbolizer CLI. They never execute the fixture.
A separate negative test changes the PE's CodeView GUID and requires rejection.

Rebuild with the x64 MSVC compiler and linker:

```
cl /nologo /c /Z7 /Od /GS- /Fosymbol_fixture.obj symbol_fixture.cpp
link /nologo /DEBUG:FULL /ENTRY:main /SUBSYSTEM:CONSOLE /NODEFAULTLIB /OPT:NOREF /INCREMENTAL:NO /OUT:symbol_fixture.exe /PDB:symbol_fixture.pdb symbol_fixture.obj
```

The controlled fixture keeps unit tests independent of whichever engine EXE
and PDB happen to be in the checkout. Actual crash symbolization still requires
the exact PDB identity embedded in that engine's PE file; no identity check is
disabled or relaxed.
