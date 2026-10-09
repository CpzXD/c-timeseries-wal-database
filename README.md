# tsdb-c

O bază de date time-series embedded în C, construită etapă cu etapă pe Windows.
În prezent sunt implementate formatul punctelor, jurnalul WAL, indexul în
memorie și interogările. Segmentele și compactarea urmează în etapele următoare.

## Construire și teste

Din Developer PowerShell for Visual Studio, în acest director:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Build-ul folosește `/W4 /WX`: avertismentele sunt erori. Rulează întotdeauna
build-ul înainte de CTest; altfel poți rula executabile rămase de la un build
anterior.

## Ce există acum

- `include/tsdb/record.h` și `src/record.c`: puncte de 92 octeți, cu CRC propriu.
- `src/binary.h`: citirea și scrierea numerelor în little-endian.
- `include/tsdb/wal_format.h` și `src/wal_format.c`: antetul fișierului,
  BEGIN v2 de 20 de octeți, BEGIN v1 și COMMIT de câte 16 octeți.
- `include/tsdb/wal.h` și `src/wal.c`: scrierea loturilor, sincronizarea cu
  Windows și recuperarea după redeschidere.
- `tests/test_wal_engine.c`: loturi multiple, reluarea lor, cozi incomplete și
  coruperea unui lot complet.
- `tests/test_wal_safety.c`: BEGIN v2 corupt, fișier străin, fișier lipsă și
  compatibilitatea cu jurnale v1.
- `tests/test_wal_fault.c`: erori simulate la scriere, la sincronizare și la
  anularea unei scrieri eșuate. Se compilează separat, cu hookuri disponibile
  numai în executabilul de test.
- `tests/test_wal_crash.c`: opt scenarii de oprire forțată a unui proces copil,
  înainte, în timpul și după scrierea lotului; backup, reparare și interogări
  după recuperare. Este compilat separat cu aceleași hookuri de test.
- `include/tsdb/tsdb.h`, `src/index.c` și `src/tsdb.c`: API-ul bazei de date,
  indexul sortat, interogările pe interval și agregările.
- `tests/test_query.c`: puncte sosite în afara ordinii, înlocuirea ultimei
  valori pentru aceeași metrică și timestamp, agregări și redeschidere.
- `tests/test_query_model.c`: comparații cu un tabel de referință, intervale
  variate și timestampuri `INT64_MIN` și `INT64_MAX`.

Fișierele noi folosesc formatul v2: `FILE_HEADER(16)`, urmat de loturi
`BEGIN(20) + count × RECORD(92) + COMMIT(16)`. Antetul fișierului și BEGIN
au CRC propriu. COMMIT conține numărul de secvență și CRC-ul calculat peste
BEGIN și toate recordurile. Jurnalele vechi v1, fără antet de fișier și cu
BEGIN de 16 octeți, rămân citibile și pot primi scrieri în același format.
Numărul de secvență începe la 1 și crește cu fiecare lot confirmat.

[`wal.h`](include/tsdb/wal.h) este interfața publică a jurnalului. Un apel
`tsdb_wal_write_batch` întoarce succes numai după ce Windows confirmă
`FlushFileBuffers`. Deschiderea nu creează și nu trunchiază fișierul. Dacă
ultimul lot este incomplet, întoarce `TSDB_ERR_INCOMPLETE`. Repararea este
un apel separat: cere un backup nou, copiază întregul WAL și apoi elimină
coada incompletă. Un BEGIN v2 cu CRC greșit este respins fără trunchiere.
Pentru v1, un BEGIN deteriorat poate semăna cu o coadă incompletă chiar după
loturi valide. Repararea oricărei cozi incomplete v1 este refuzată, iar
fișierul rămâne nemodificat. Jurnalele v1 complete rămân citibile și pot primi scrieri.
`tsdb_wal_replay` cheamă vizitatorul o dată pentru fiecare lot valid.
Limita actuală este 4096 puncte pe lot.

`tsdb_wal_open` permite un singur writer; cititorii pot deschide împreună
fișierul în modul read-only. Dacă un
apel de scriere întoarce `TSDB_ERR_IO`, închide jurnalul și redeschide-l înainte
de a decide dacă reîncerci lotul; rezultatul unei erori de I/O poate fi incert.
Confirmarea prin `FlushFileBuffers` nu promite protecție absolută împotriva
oricărei defecțiuni hardware.
Testul de erori este explicat în [`texte/WAL_ERORI_IO.md`](texte/WAL_ERORI_IO.md).
Testele de oprire a procesului sunt explicate pas cu pas în
[`texte/WAL_OPRIRE_PROCES.md`](texte/WAL_OPRIRE_PROCES.md). Toate cele 9 teste
CTest trec; testul `wal_process_crash` conține 8 scenarii.

Pentru explicația pas cu pas a acestei etape, citește
[`texte/WAL_ETAPA_2.md`](texte/WAL_ETAPA_2.md). Cursul mai amplu este în
[`texte/CURS_TSDB_C.md`](texte/CURS_TSDB_C.md).

## Comenzi CLI disponibile

```powershell
.\build\Debug\tsdb.exe init date.bin
.\build\Debug\tsdb.exe write date.bin cpu 1000 42.5
.\build\Debug\tsdb.exe write date.bin cpu 500 10
.\build\Debug\tsdb.exe query date.bin cpu 0 2000
.\build\Debug\tsdb.exe aggregate date.bin cpu 0 2000
```

`init` creează doar un fișier nou și refuză să suprascrie unul existent.
`query` și `aggregate` deschid doar pentru citire; un nume de fișier lipsă
produce eroare și nu creează baza. Dacă apare o coadă incompletă, folosește
explicit `repair` cu un nume de backup care nu există deja:

```powershell
.\build\Debug\tsdb.exe repair date.bin date.inainte-de-repair.bin
```

Un jurnal v1 incomplet cere diagnostic separat: `repair` îl refuză, fiindcă
v1 nu poate verifica independent câmpul `count` din BEGIN.

Intervalele sunt inclusive. Pentru aceeași metrică și același timestamp,
ultima scriere confirmată câștigă în funcționarea normală. La recuperare sunt
reluate loturile complete și valide: un lot poate fi găsit integral chiar
dacă procesul a fost oprit înainte ca apelantul să primească succes. Lipsa
confirmării cere verificarea datelor înainte de reîncercare.
Indexul este refăcut din WAL la fiecare
deschidere și se află integral în memorie; interogarea folosește căutare binară,
iar inserarea poate muta elemente. Aceasta este implementarea etapei 3, nu
încă motorul cu segmente pe disc.

Raportul actual, cu explicații și limite, este în
[`texte/REVIEW_TOTAL_2026-10-07.md`](texte/REVIEW_TOTAL_2026-10-07.md).
`.clang-format` păstrează formatarea consecventă a fișierelor C și a headerelor.
