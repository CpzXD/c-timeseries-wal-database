# Etapa 2: jurnalul WAL

WAL înseamnă *Write-Ahead Log*: înainte ca indexul din memorie să fie
actualizat, punctele sunt păstrate într-un jurnal pe disc. După repornire,
indexul este reconstruit prin citirea jurnalului.

## De ce avem BEGIN și COMMIT?

Un fișier nou are un antet de 16 octeți. Pentru un lot v2 cu două puncte,
octeții care urmează sunt așezați astfel:

```text
BEGIN v2 (20) | record 1 (92) | record 2 (92) | COMMIT (16)
```

BEGIN v2 își verifică propriile câmpuri prin CRC și spune câte recorduri
urmează și ce număr de secvență are lotul. Formatul vechi v1 nu are antet de
fișier și folosește un BEGIN de 16 octeți fără CRC propriu. COMMIT
repetă secvența și stochează CRC-ul întregului prefix, de la BEGIN până la
ultimul record. Un lot este aplicat numai dacă toate piesele există și trec
verificările. Dacă procesul se oprește după recordul 2, dar înainte de COMMIT,
lotul este incomplet și nu se aplică.

CRC-ul nu este criptografie. El descoperă modificări accidentale ale octeților.
Fiecare record are și propriul CRC; cel din COMMIT protejează lotul ca întreg,
inclusiv BEGIN.

## Ce face fiecare funcție?

1. `tsdb_wal_create(path, &wal)` creează un fișier v2 nou, cu antet protejat
   prin CRC. `tsdb_wal_open(path, mode, &wal)` citește doar un fișier existent.
   Dacă ultimul lot este incomplet, întoarce `TSDB_ERR_INCOMPLETE` fără să
   modifice fișierul. Un lot corupt întoarce eroare și rămâne pe disc.
2. `tsdb_wal_write_batch(wal, points, count, &sequence)` verifică toate
   punctele în memorie, construiește BEGIN, recordurile și COMMIT, apoi adaugă
   lotul la sfârșitul fișierului. Succesul este raportat după
   `FlushFileBuffers`, care cere sistemului de operare să sincronizeze fișierul.
3. `tsdb_wal_replay(wal, callback, context, &count)` validează din nou jurnalul
   și trimite fiecare lot complet către callback. Pointerul `points` primit de
   callback trăiește doar în timpul apelului; copiază datele dacă le păstrezi.
4. `tsdb_wal_repair(path, backup_path, &discarded)` când găsește o coadă
   incompletă, copiază mai întâi întregul fișier într-un backup nou, apoi
   elimină coada. Dacă fișierul este complet, raportează zero octeți eliminați
   și nu creează backup. Un backup cu
   același nume nu este suprascris. Pentru v1, o valoare `count` deteriorată
   poate imita o coadă incompletă, inclusiv după loturi valide. De aceea orice
   coadă incompletă v1 este refuzată, fără backup sau trunchiere.
5. `tsdb_wal_close(wal)` eliberează handle-ul și memoria.

Numărul maxim de puncte dintr-un lot este `TSDB_WAL_MAX_BATCH_POINTS` (4096).
Limita împiedică un BEGIN deteriorat să provoace o alocare uriașă la recuperare.

## Cum citești testul nou?

În `tests/test_wal_engine.c`, două loturi sunt scrise și apoi citite după
redeschidere. Testul adaugă apoi 7 octeți din următorul BEGIN, ca și cum
procesul s-ar fi oprit în timpul scrierii. Deschiderea raportează coada fără
să schimbe fișierul; repararea face un backup, apoi elimină cei 7 octeți.
Un test asemănător scrie BEGIN, recordul și doar jumătate din COMMIT.
La final, testul modifică un bit
într-un lot complet: deschiderea trebuie să raporteze eroare de checksum și să
păstreze fișierul pentru diagnostic.

Testele simulează o coadă incompletă prin scrieri scurte. Ele nu demonstrează
comportamentul pentru orice întrerupere a curentului sau defecțiune de disc.

## Ce urmează?

Etapa 3 a adăugat un index în memorie. La deschidere, callback-ul de replay
inserează punctele confirmate în index, iar interogările caută după metrică și
interval de timp. Comenzile CLI `init`, `write`, `query` și `aggregate` permit
o demonstrație fără un program C separat. Segmentele pe disc urmează.
