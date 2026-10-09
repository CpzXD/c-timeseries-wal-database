# Caiet de laborator — TSDB în C

Acest caiet este partea practică a cursului. Citește `CURS_TSDB_C.md` pentru
harta completă. Aici găsești **ce fișier deschizi, ce scrii, cum verifici și de
ce**. Tastează tu codul și oprește-te după fiecare exercițiu pentru review.
Fragmentele de mai jos arată forma unei soluții; nu sunt fișiere complete de
lipit peste proiect.

## Cum folosești caietul

1. Deschide în VS Code folderul `C:\Users\username\Documents\baza de date in C`.
2. Deschide doar fișierul indicat de lecția curentă.
3. Citește exemplul, apoi tastează fragmentul și explică-ți fiecare linie.
4. Salvează cu `Ctrl+S` **înainte** de compilare.
5. În terminalul din folderul proiectului rulează:

```powershell
cmake --build build --config Debug
```

6. Rulează testul indicat. Dacă eșuează, citește prima linie `FAIL`: ea arată
   comportamentul care lipsește. Trimite-mi codul sau eroarea pentru review.

Nu modifica testele existente pentru a ascunde o eroare. Putem adăuga teste
noi când descoperim un caz pe care nu îl acoperă.

---

## Laboratorul 0 — Înțelegi ce compilezi

**Fișiere de citit:** `CMakeLists.txt`, `include/tsdb/record.h`, `src/main.c`.

### Ce scrii

Nu scrii cod încă. Rulezi:

```powershell
cmake --build build --config Debug
.\build\Debug\tsdb.exe --version
```

**Rezultat așteptat:** `tsdb-c 0.1.0`.

### De ce

`CMakeLists.txt` spune compilatorului să creeze:

```text
src/record.c → tsdb_core.lib
src/main.c + tsdb_core.lib → tsdb.exe
tests/test_record.c + tsdb_core.lib → record_tests.exe
```

În `record.h`, `typedef struct` definește forma unui punct. Declarațiile
funcțiilor sunt promisiuni: „există o funcție cu această intrare și această
ieșire”. Corpul funcției este în `record.c`.

`/W4 /WX` înseamnă că MSVC afișează multe avertismente și le tratează drept
erori. Vrem să reparăm avertismentele, fiindcă în codul de stocare un detaliu
mic poate strica date.

---

## Laboratorul 1 — Formatul binar al unui punct

**Fișiere:** `include/tsdb/record.h`, `src/record.c`, `tests/test_record.c`.

### 1.1 Contractul funcției

În `record.h` există:

```c
TsdbStatus tsdb_record_encode(const TsdbPoint *point,
                              uint8_t out[TSDB_RECORD_SIZE]);
```

În C, parametrul `out[TSDB_RECORD_SIZE]` se comportă aici ca un pointer la
primul octet. `point` este `const`: funcția citește punctul, dar nu îl schimbă.
`TsdbStatus` spune dacă a reușit sau de ce nu a reușit.

**Exemplu:** `encode(NULL, raw)` trebuie să întoarcă `TSDB_ERR_ARGUMENT`.
Nu avem voie să încercăm `point->metric` când `point == NULL`.

### 1.2 Ce înseamnă un offset

Recordul are 92 de octeți, `record[0]` până la `record[91]`. Offsetul este
numărul poziției de la început. De exemplu, `&record[24]` este adresa de la
care începe numele metricii. Pentru `cpu`, copiem 3 octeți de acolo:

```c
memcpy(&record[24], point->metric, name_length);
```

`memcpy(destinație, sursă, număr_de_octeți)` copiază exact câți octeți cerem.
Nu caută automat `\0` și nu știe că datele sunt text.

### 1.3 De ce scriem little-endian manual

În record, timestampul ocupă pozițiile 8–15. Pentru fiecare octet `byte`
din 0–7, punem octetul corespunzător al numărului:

```c
record[8 + byte] = (uint8_t)(timestamp_bits >> (8 * byte));
```

`>>` mută biții spre dreapta; conversia la `uint8_t` păstrează ultimii 8 biți.
Pentru `0x0102030405060708`, primul octet scris este `08`, apoi `07`, până la
`01`. Aceasta este o decizie a **formatului nostru**, nu ceva ce lăsăm la
alegerea compilatorului.

Pentru valoarea `double`, folosim `memcpy` către un `uint64_t` înainte de
mutări. Așa păstrăm biții lui `1.5` sau `-0.0`; un cast numeric ar pierde
informație.

### 1.4 Unde ești acum

În codul tău, `encode` validează intrarea, scrie toate câmpurile, calculează
CRC-ul și întoarce `TSDB_OK`. Testul cu layoutul exact trece. `decode` verifică
deja pointerii, `TSR1`, versiunea, lungimea numelui și octeții rezervați.

**Următoarea bucată de tastat în `src/record.c`:** în funcția
`tsdb_record_decode`, după verificarea `in[6]` și `in[7]`, citește CRC-ul
stocat:

```c
uint32_t stored_crc =
    (uint32_t)in[88] |
    ((uint32_t)in[89] << 8) |
    ((uint32_t)in[90] << 16) |
    ((uint32_t)in[91] << 24);

(void)stored_crc; /* doar până la exercițiul următor */
```

**De ce:** la `encode` ai împărțit un număr de 32 de biți în patru octeți.
Acum îi unești la loc. `<<` mută octetul la poziția sa; `|` combină părțile.
Conversia la `uint32_t` înaintea mutării împiedică operații nedorite pe un
`int` cu semn. Dacă octeții sunt `92 EF 69 7A`, numărul reconstruit este
`0x7A69EF92`.

**Verificare:** compilează. Testele `round_trip` și `corruption` vor rămâne
roșii; încă nu comparăm CRC-ul. `(void)stored_crc` ține compilatorul mulțumit
temporar.

### 1.5 Recalculezi CRC-ul

În `decode`, calculează CRC-ul din `in[0]` până la `in[87]`, folosind aceeași
buclă pe care o ai în `encode`. Diferența este sursa octeților: `in[pos]` în
loc de `record[pos]`. După complementarea finală, compară:

```c
if (computed_crc != stored_crc) {
    return TSDB_ERR_CHECKSUM;
}
```

Elimină `(void)stored_crc`. Dacă cineva a schimbat un octet al valorii, noul
CRC diferă de cel stocat. Dacă `TSR1` este greșit, funcția întoarce mai întâi
`TSDB_ERR_FORMAT`, fiindcă verificarea antetului se află înaintea CRC-ului.

**Verificare:** rulează `.\build\Debug\record_tests.exe`. Testul
`test_corruption` trebuie să treacă; `test_round_trip` încă așteaptă citirea
câmpurilor. După ce funcționează, vom extrage calculul CRC într-o funcție
privată comună pentru `encode` și `decode`, ca să avem o singură implementare.

### 1.6 Reconstruiești timestampul și valoarea

Pentru timestamp, pornești de la un `uint64_t` zero și adaugi fiecare octet:

```c
uint64_t timestamp_bits = 0;
for (size_t byte = 0; byte < 8; byte++) {
    timestamp_bits |= ((uint64_t)in[8 + byte]) << (8 * byte);
}
```

`|=` înseamnă `timestamp_bits = timestamp_bits | ...`. Conversia la `uint64_t`
se face înainte de `<<`, altfel mutarea cu 56 de biți nu ar încăpea într-un
`int` obișnuit. Repeți ideea pentru valoare, folosind `in[16 + byte]`.

Copiază **biții** în câmpurile unui `TsdbPoint` temporar cu `memcpy`. Pentru
timestampul negativ evităm o conversie nesigură dintr-un `uint64_t` prea mare
în `int64_t`; copierea biților păstrează reprezentarea de pe platforma noastră.

### 1.7 Reconstruiești numele și termini `decode`

Ia `name_length = in[5]`. Copiază numai acei octeți din `in[24]` în
`temp.metric`, apoi pune `temp.metric[name_length] = '\0'`. Verifică faptul că
octeții rămași din zona 24–87 sunt zero și că fiecare caracter copiat respectă
regula numelui. Verifică și `isfinite(temp.value)`.

```c
TsdbPoint temp = {0};
/* reconstruiești câmpurile și le validezi aici */
*out = temp;
return TSDB_OK;
```

**De ce obiect temporar?** Dacă recordul este invalid, funcția iese înainte de
`*out = temp`; obiectul primit de la apelant rămâne intact.

**Criteriu de ieșire:** toate testele din `record_tests.exe` trec. Mai adăugăm
teste pentru nume invalid în fișier, padding nenul și trunchiere înainte de a
folosi recordul pentru date reale.

---

## Laboratorul 2 — Primul fișier și jurnalul de scriere

**Scop:** înțelegi diferența dintre „am format un record” și „l-am salvat
sigur”. Lucrăm în `src/wal.c`, `include/tsdb/wal.h`, `tests/test_wal.c`, pe care
le vom crea după Laboratorul 1.

### 2.1 Exercițiu de învățare: scrie și citește un record

Într-un mic test temporar, deschizi un fișier binar, encodezi un punct și scrii
exact 92 de octeți. Modelul operațiilor este:

```c
uint8_t raw[TSDB_RECORD_SIZE];
/* verifici că tsdb_record_encode(&point, raw) == TSDB_OK */
/* deschizi fișierul în mod binar pentru scriere */
size_t written = fwrite(raw, 1, sizeof raw, file);
/* verifici că written == sizeof raw și verifici fclose */
```

**De ce `fwrite(raw, 1, sizeof raw, file)`?** Argumentele sunt „dimensiunea
unei unități” și „numărul de unități”; cu unitate de 1 octet, rezultatul este
numărul de octeți efectiv scriși. O scriere scurtă este eroare.

**Exemplu:** două recorduri complete ocupă 184 de octeți. Un fișier de 130 de
octeți conține unul complet și un început de al doilea.

Acesta este **un exercițiu**, nu forma finală a WAL-ului. `fclose` și `fflush`
nu sunt contractul nostru final de durabilitate și nu avem încă batchuri atomice.

### 2.2 Citește înapoi și tratează finalul incomplet

Într-o buclă, încerci să citești 92 de octeți. Dacă obții 92, apelezi
`decode`. Dacă obții 0 și ai ajuns la EOF, jurnalul s-a terminat normal. Dacă
obții între 1 și 91 la final, ai o ultimă scriere incompletă.

```c
size_t got = fread(raw, 1, sizeof raw, file);
if (got == sizeof raw) {
    /* decode și procesează */
} else if (got == 0 && feof(file)) {
    /* sfârșit normal */
} else {
    /* eroare de citire sau coadă incompletă: distinge cu ferror/feof */
}
```

**De ce distincția?** O citire scurtă poate însemna finalul fișierului sau o
eroare I/O; nu vrem să ascundem o problemă reală.

### 2.3 Forma finală: batch atomic

Un jurnal format doar din recorduri nu spune dacă un batch de trei puncte a
fost terminat. Vom defini un cadru binar explicit:

```text
BEGIN:  magic TBG1 (4) + count uint32 LE (4) + sequence uint64 LE (8)
DATA:   count × recorduri TSR1 de 92 de octeți
COMMIT: magic TCM1 (4) + sequence uint64 LE (8) + CRC32 LE (4)
```

CRC-ul de COMMIT acoperă întregul BEGIN și DATA. În `wal.h` vom declara
operațiile `wal_append_batch` și `wal_replay`. Nu scriem structurile C direct
în fișier; codăm fiecare câmp la offsetul stabilit, ca la `record`.

**Exemplu:** procesul cade după al doilea punct dintr-un batch de trei.
Lipsește COMMIT-ul valid, deci la repornire ignorăm întregul batch. Dacă COMMIT
este valid, aplicăm toate cele trei puncte o singură dată. Impunem o limită
maximă pentru `count` înainte de alocare, ca un fișier corupt să nu ceară o
cantitate uriașă de memorie.

### 2.4 Când putem spune „scriere confirmată”

Pe Windows, `FlushFileBuffers` solicită scrierea datelor tamponate ale unui
fișier. În componenta Windows de I/O vom folosi handle-uri și vom verifica
fiecare rezultat al operațiilor de deschidere, scriere și flush. `write_batch`
întoarce `TSDB_OK` doar după scrierea completă și flushul COMMIT-ului.

**Exemplu:** dacă `WriteFile` raportează mai puțini octeți decât am cerut sau
`FlushFileBuffers` eșuează, apelul întoarce eroare, nu succes. Nu promitem
rezistență la orice defect fizic al unității de stocare.

**Teste:** jurnal gol, un batch, două batchuri, ultimul batch tăiat la fiecare
offset important, CRC greșit în mijloc, eroare I/O simulată. Testele de crash
vin în Laboratorul 7.

---

## Laboratorul 3 — Indexul din memorie

**Fișiere:** `src/index.h`, `src/index.c`, `tests/test_query.c`.

### 3.1 Prima reprezentare

Începem cu o colecție de serii. O serie înseamnă toate punctele cu același
nume de metrică. În interiorul seriei avem puncte și numărul commitului care
le-a făcut vizibile.

```c
typedef struct {
    TsdbPoint point;
    uint64_t sequence;
} TsdbEntry;

typedef struct {
    TsdbEntry *items;
    size_t length;
    size_t capacity;
} TsdbVector;
```

`length` este numărul folosit, `capacity` este spațiul alocat. Pentru început
căutarea metricii poate fi liniară; o optimizăm numai când măsurătorile arată
că merită.

### 3.2 Creșterea unui vector

Modelul sigur pentru `realloc` este:

```c
if (vector->length == vector->capacity) {
    size_t next = vector->capacity == 0 ? 16 : vector->capacity * 2;
    TsdbEntry *grown = realloc(vector->items, next * sizeof *grown);
    if (grown == NULL) {
        return TSDB_ERR_NO_MEMORY;
    }
    vector->items = grown;
    vector->capacity = next;
}
vector->items[vector->length++] = entry;
```

**De ce pointer temporar `grown`?** Dacă `realloc` eșuează și am scrie direct
rezultatul în `vector->items`, am pierde adresa memoriei vechi. În implementare
verificăm și că `capacity * 2` și `next * sizeof` nu depășesc `SIZE_MAX`.
Adăugăm `<stdlib.h>` pentru `realloc` și `free`.

### 3.3 Puncte întârziate și duplicate

Exemplu de sosire: timestampurile 10, 30, 20. La interogare le prezentăm
10, 20, 30. Pentru aceeași pereche `(metric, timestamp)`, păstrăm valoarea din
ultimul batch confirmat: `cpu@10=20`, apoi `cpu@10=25` → răspunsul este 25.

**Ce scrii:** funcții pentru căutarea unei serii, adăugare, sortare/căutare și
eliberarea completă a memoriei. Nu publicăm o schimbare dintr-un batch în
index înainte de COMMIT-ul confirmat.

**Teste:** insert în ordine, insert întârziat, duplicate, `realloc` eșuat prin
injectare, reconstrucție din WAL.

---

## Laboratorul 4 — Interogări și agregări

**Fișiere:** `include/tsdb/tsdb.h`, `src/tsdb.c`, `src/main.c`, `tests/test_query.c`.

### 4.1 API-ul public

Un API înseamnă funcțiile pe care alt program are voie să le apeleze. Avem
un tip opac `Tsdb`: utilizatorul primește un pointer, dar nu poate modifica
direct structurile interne.

```c
typedef struct Tsdb Tsdb;
TsdbStatus tsdb_open(const char *path, Tsdb **out_db);
TsdbStatus tsdb_write_batch(Tsdb *db, const TsdbPoint *points, size_t count);
TsdbStatus tsdb_query_range(Tsdb *db, const char *metric,
                            int64_t start_ms, int64_t end_ms,
                            TsdbPoint **out_points, size_t *out_count);
void tsdb_free_points(TsdbPoint *points);
TsdbStatus tsdb_close(Tsdb *db);
```

**De ce `Tsdb **out_db`?** Funcția trebuie să schimbe pointerul apelantului,
nu doar o copie locală a lui. Vom documenta cine deține memoria returnată și
cine o eliberează. Codurile de eroare includ argument invalid, I/O, format,
checksum, lipsă memorie și bază ocupată.

### 4.2 Intervalele `[start, end]`

Condiția este `timestamp >= start_ms && timestamp <= end_ms`. Ambele limite
sunt incluse, iar `[30, 30]` poate căuta exact timestampul 30.

**Exemplu:** avem puncte la 10, 20, 30. `[10, 30]` întoarce toate trei punctele.
Pentru două rezultate alăturate fără dubluri alegem `[10, 29]` și `[30, 40]`.

### 4.3 `count`, `min`, `max`, `avg`

Pentru 10, 20, 30 obținem 3, 10, 30, 20. Actualizăm media progresiv:

```c
count++;
double divisor = (double)count;
mean += value / divisor - mean / divisor;
```

**De ce:** formula permite o medie fără să păstrăm separat toate valorile și
evită, în multe cazuri, un total intermediar foarte mare. Pentru zero puncte,
`count=0`, iar `min/max/avg` sunt zero; `count` indică lipsa valorilor.

### 4.4 CLI-ul

`main.c` va parsa comenzi text și va apela API-ul. Exemple de interfață
planificată:

```text
tsdb init data
tsdb write data cpu.percent 1727532000000 42.5
tsdb query data cpu.percent 1727532000000 1727535600000
tsdb aggregate data cpu.percent 1727532000000 1727535600000
```

Vom folosi `strtoll` și `strtod`, verificând că întregul argument a fost
consumat și că nu apare overflow. CLI-ul afișează mesaje de eroare clare.

**Teste:** interval gol, metrică inexistentă, timestamp invalid, `start > end`,
duplicate, medie cu valori pozitive și negative.

---

## Laboratorul 5 — Segmente imuabile pe disc

**Fișiere noi:** `src/segment.c`, `include/tsdb/segment.h`, teste de segment.

### 5.1 Ce problemă rezolvăm

WAL-ul și indexul activ nu pot crește la infinit. La un prag controlat scriem
un **segment**: puncte sortate după `(metrică, timestamp, sequence)`. După ce
segmentul a fost finalizat, nu îl modificăm.

**Exemplu:** segmentul A conține ianuarie, segmentul B martie. O interogare
doar pentru martie poate sări A după ce citește intervalele din antet.

### 5.2 Primul segment

Începem cu un fișier simplu: un antet cu magic/versiune/count și un șir de
recorduri. Scriem într-un fișier temporar, verificăm fiecare `fwrite`, îl
sincronizăm și abia apoi îl declarăm activ în manifest. Nu scriem un `struct`
direct pe disc; fiecare câmp din antet primește offset și ordine a octeților.

**Ce scrii:** comparatorul pentru `qsort`, writerul de segment, readerul și
verificarea antetului. Exemplu de criteriu pentru comparator: întâi `strcmp`
pentru metrică, apoi compararea timestampurilor fără scădere (scăderea a două
`int64_t` ar putea face overflow), apoi `sequence`.

### 5.3 Index rar

După ce citirea secvențială funcționează, păstrăm offsetul fiecărui bloc de,
de exemplu, 256 de puncte. Pentru o căutare sărim la blocul relevant și citim
doar o parte din fișier. Acesta este un **index rar**; nu avem nevoie de o
intrare în index pentru fiecare punct.

**Exemplu:** dintr-un milion de puncte, indexul ne duce lângă intervalul
cerut, în loc să citim de la primul punct.

### 5.4 Manifestul și checkpointul

Manifestul listează segmentele active și poziția WAL până la care datele sunt
deja în segmente. La repornire citim manifestul și refacem din WAL doar datele
mai noi. Actualizarea manifestului trebuie să fie recuperabilă: scriem o
versiune nouă, o verificăm, o sincronizăm, apoi o activăm.

**Exemplu de crash:** segmentul nou există, dar manifestul vechi este încă
activ. La repornire folosim manifestul vechi și WAL-ul; segmentul incomplet
activat nu poate face datele să dispară.

**Teste:** 0/1/multe puncte, segment tăiat, index greșit, crash înainte și
după activarea manifestului, interogări care unesc indexul activ cu segmentele.

---

## Laboratorul 6 — Compactare și retenție

**Fișiere:** `src/compaction.c`, CLI `compact`, teste de integrare.

### 6.1 Îmbinarea a două segmente sortate

Ținem câte un cursor în fiecare segment. Comparăm cheile curente, emitem
cheia mai mică și avansăm cursorul potrivit. Dacă găsim aceeași pereche
`(metrică, timestamp)`, păstrăm intrarea cu `sequence` mai mare.

```text
A: cpu@10=20, cpu@30=40
B: cpu@10=25, cpu@20=35
rezultat: cpu@10=25, cpu@20=35, cpu@30=40
```

**De ce:** după compactare, interogarea citește mai puține fișiere și dublurile
vechi nu mai ocupă spațiu. Testăm că rezultatele interogărilor sunt identice
înainte și după compactare.

### 6.2 Schimbarea sigură a fișierelor

Ordinea logică este: creează segment nou → termină și verifică scrierea →
activează manifestul nou → șterge segmentele care nu mai sunt active. Dacă
procesul se oprește între pași, manifestul arată ce fișiere sunt autoritare.
Pe Windows vom verifica operațiile de înlocuire a fișierelor și tratarea
erorilor, fără să presupunem că un simplu `rename` rezolvă orice caz.

### 6.3 Retenția

Definim un prag temporal: `cutoff_ms = now_ms - retention_ms`, verificând
overflow. Punctele mai vechi de prag nu apar în interogări și sunt eliminate
fizic la compactare. Luăm timpul de referință ca parametru în teste, ca testul
să nu depindă de ceasul real.

**Exemplu:** păstrăm 30 de zile; un punct de acum 31 de zile expiră, unul de
acum 29 de zile rămâne.

**Teste:** segmente cu date de ambele părți ale pragului, duplicate, crash la
fiecare etapă a activării, spațiu eliberat numai după activare sigură.

---

## Laboratorul 7 — Crash, performanță și demonstrație

### 7.1 Testezi ce se întâmplă când procesul moare

Un program de test pornește un proces copil, îi cere să scrie un batch și îl
oprește forțat la puncte controlate: după BEGIN, la jumătatea DATA, după
COMMIT, după flush. Repornește baza și verifică toate batchurile confirmate.

**De ce:** închiderea normală a programului nu simulează un crash. Un test
fiabil trebuie să forțeze oprirea când operația este incompletă.

### 7.2 Compari cu un model simplu

Pentru teste, păstrezi într-o structură simplă din memorie răspunsul corect.
Generezi multe operații, le aplici atât modelului, cât și bazei tale, apoi
compari interogările. Modelul poate fi lent: rolul lui este corectitudinea.

**Exemplu:** 10.000 de puncte aleatorii, inclusiv timestampuri întârziate și
duplicate, trebuie să dea aceleași rezultate după restart și compactare.

### 7.3 Măsori fără să te păcălești

Notezi puncte/secundă, timp de interogare, timp de deschidere și spațiu pe
disc. Fiecare rezultat include numărul de puncte, mărimea batchului, tipul
discului și politica de flush. Fără aceste detalii, două cifre de performanță
nu sunt comparabile.

### 7.4 Adaugi un colector Windows

Colectorul este separat de motor. Pentru RAM, o lecție va folosi
`GlobalMemoryStatusEx` și va scrie metrici precum `system.ram.used_bytes`.
Pentru CPU citim două eșantioane ale timpilor sistemului și calculăm o
diferență; un singur eșantion nu dă procentul CPU.

**Demo final:** pornești colectorul, aduni date, închizi și redeschizi baza,
interoghezi un interval, rulezi compactarea și arăți testele de crash.

---

## Regula de lucru pentru următoarea sesiune

Începem din Laboratorul 1.4: citești CRC-ul stocat în `decode`. Nu scrii deodată
WAL-ul și indexul. După fiecare fragment îmi trimiți fie codul, fie rezultatul
compilării și al testelor. Eu îți explic exact ce a făcut codul și ce corectăm.
