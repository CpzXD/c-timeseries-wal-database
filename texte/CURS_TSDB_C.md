# Curs practic: construim o bază de date time-series în C

**Pentru instrucțiuni concrete de scriere a codului**, deschide și
[`CAIET_DE_LABORATOR.md`](CAIET_DE_LABORATOR.md). Acest fișier explică
obiectivele și ordinea lecțiilor; caietul arată ce fișier deschizi, ce cod scrii,
de ce îl scrii și ce rezultat trebuie să verifici.

Acest curs este harta proiectului, nu o soluție de copiat. Tu scrii fiecare
componentă; eu explic, îți dau un exercițiu mic, îți revizuiesc codul și te ajut
să depanezi. Avansăm numai când criteriul lecției curente este verificat.

**Ținta v1:** o bibliotecă C și un CLI pe Windows care păstrează metrici de sistem,
recuperează scrierile confirmate după oprirea procesului și interoghează valori
după metrică și interval. Lucrăm cu un singur proces și un singur writer. Nu
adăugăm SQL, server HTTP sau sincronizare între calculatoare în v1.

**Ritmul estimat:** 18–26 de săptămâni la 10+ ore pe săptămână. Este o estimare,
nu o promisiune. Stabilitatea contează mai mult decât bifarea unei date.

**Lecția adăugată după implementarea WAL-ului și a interogărilor:**
[`WAL_OPRIRE_PROCES.md`](WAL_OPRIRE_PROCES.md) explică cele opt teste de
oprire forțată și diferența dintre un COMMIT complet și succesul primit de apelant.

## 0. Cum înveți și cum verificăm

Fiecare lecție are aceeași ordine:

1. **Ideea:** explicăm conceptul folosind o analogie și un exemplu numeric.
2. **Contractul:** stabilim exact intrarea, ieșirea și erorile funcției.
3. **Exercițiul tău:** implementezi o singură bucată mică.
4. **Verificarea:** compilezi cu avertismente stricte și rulezi testele relevante.
5. **Review:** discutăm corectitudinea, cazurile-limită și claritatea codului.

Un test roșu este o informație, nu un eșec personal. Întrebarea este: ce
comportament lipsește și care este cel mai mic pas pentru a-l adăuga?

Comenzi uzuale, rulate din folderul proiectului:

```powershell
cmake --build build --config Debug
.\build\Debug\record_tests.exe
ctest --test-dir build -C Debug --output-on-failure
```

`ctest` rulează toate testele înregistrate în CMake, inclusiv `record_tests`.
Starea actuală și explicațiile implementării sunt în `REVIEW_TOTAL_2026-10-07.md`.

## 1. Fundamentele pe care le întâlnim în cod

### 1.1 Bit, octet și hexazecimal

Un bit este 0 sau 1. Opt biți formează un octet. `uint8_t` ocupă exact un octet.
Scriem octeții în hexazecimal: `0xFF` reprezintă 255, iar `0x08` reprezintă 8.
Exemplu: octetul `0x08` este `00001000` în binar.

**De ce:** fișierele conțin octeți, nu structuri C. Pentru a diagnostica o
corupere trebuie să putem arăta exact care octet este greșit.

### 1.2 Variabile, pointeri și structuri

`TsdbPoint` grupează numele metricii, momentul și valoarea. `TsdbPoint *point`
este adresa unui astfel de obiect. `point->value` citește un câmp prin pointer.
`NULL` înseamnă că pointerul nu indică un obiect valid.

**Exemplu:** dacă cineva apelează `encode(NULL, out)`, funcția întoarce
`TSDB_ERR_ARGUMENT` înainte să încerce `point->metric`.

### 1.3 Tipuri cu dimensiune fixă

`uint8_t` = 8 biți fără semn; `uint32_t` = 32; `uint64_t` = 64; `int64_t` = 64
cu semn. `double` ocupă 8 octeți în mediul nostru și păstrează valori reale.
`size_t` este tipul folosit pentru dimensiuni și indici de memorie.

**De ce:** „un `int`” nu este o promisiune suficient de precisă pentru un format
de fișier. Formatul nostru trebuie să însemne același lucru la fiecare citire.

### 1.4 Header, cod sursă, bibliotecă și teste

- `include/tsdb/record.h` declară structuri, constante, erori și funcții.
- `src/record.c` implementează funcțiile.
- `src/main.c` construiește programul `tsdb.exe`.
- `tests/test_record.c` verifică comportamentul.
- `CMakeLists.txt` descrie cum se compilează totul.

Headerul nostru se include cu ghilimele: `"tsdb/record.h"`. Headerurile C
standard se includ cu paranteze unghiulare:

| Header | Folosit pentru | Exemplu |
| --- | --- | --- |
| `<stdint.h>` | numere cu dimensiune fixă | `uint8_t` |
| `<stddef.h>` | dimensiuni și pointer nul | `size_t`, `NULL` |
| `<string.h>` | octeți și memorie | `memcpy`, `memcmp` |
| `<math.h>` | proprietăți numerice | `isfinite` |
| `<stdio.h>` | afișare și fișiere în lecții viitoare | `fprintf` |

MSVC transformă `record.c` în `tsdb_core.lib`; apoi legăm acea bibliotecă la
`tsdb.exe` și `record_tests.exe`. VS Code este editorul, MSVC este compilatorul,
iar CMake descrie procesul de construire.

## 2. Modulul I — Un punct devine 92 de octeți (săptămânile 1–2)

**Întrebarea modulului:** cum păstrăm o măsurătoare într-un format pe care îl
putem recunoaște și verifica mai târziu?

### Lecția 1: definim un contract binar

Un record are 92 de octeți, numerotați 0–91:

| Offset | Octeți | Conținut | Motiv |
| ---: | ---: | --- | --- |
| 0–3 | 4 | ASCII `TSR1` | recunoaștem formatul |
| 4 | 1 | versiune `1` | putem schimba formatul în viitor |
| 5 | 1 | lungimea numelui, 1–63 | știm câte caractere citim |
| 6–7 | 2 | zero | spațiu rezervat, verificabil |
| 8–15 | 8 | timestamp `int64`, little-endian | momentul măsurătorii |
| 16–23 | 8 | biții lui `double`, little-endian | valoarea exactă |
| 24–87 | 64 | numele și zerouri de completare | lungime fixă |
| 88–91 | 4 | CRC-32/IEEE, little-endian | detectăm coruperea accidentală |

**Exemplu:** pentru numele `cpu`, octetul 5 este `03`, iar pozițiile 24–26
conțin `63 70 75` în hexazecimal. Celelalte poziții din zona numelui sunt zero.

**De ce nu `fwrite(&point, sizeof point, ...)`?** Compilatorul poate organiza
structura pentru memorie, inclusiv cu spații de aliniere. Fișierul trebuie să
respecte contractul nostru de 92 de octeți, nu reprezentarea accidentală a
structurii în RAM.

### Lecția 2: validăm datele înainte de codare

Numele acceptă litere ASCII, cifre, `_`, `-` și `.`, are 1–63 caractere și este
terminat cu `\0` în cei 64 de octeți ai structurii. Valoarea trebuie să fie
finită. Pointerii nu pot fi `NULL`.

**Exemple:** `system.cpu.percent` este valid; `cpu percent` nu este; `INFINITY`
nu este o valoare permisă. Parcurgem cel mult 64 de poziții, fiindcă `strlen`
ar putea continua în afara obiectului dacă lipsește terminatorul.

### Lecția 3: scriem octeții în ordine stabilită

Little-endian înseamnă că punem primul octetul cel mai puțin semnificativ.
Numărul demonstrativ `0x0102030405060708` devine:

```text
08 07 06 05 04 03 02 01
```

Pentru `double`, folosim `memcpy` ca să păstrăm biții. O conversie numerică la
`uint64_t` ar transforma `1.5` în `1` și ar pierde informația. `-0.0` și
`0.0` se compară numeric egal, dar au biți diferiți; testul verifică această
diferență.

### Lecția 4: calculăm CRC-ul

CRC-32/IEEE produce un număr de 32 de biți din octeții 0–87. Dacă schimbăm
accidental un octet, CRC-ul recalculat va fi de regulă diferit. CRC-ul nu este
criptare și nu garantează că un atacator nu a modificat datele.

**Exemplu:** pentru recordul de test cu `cpu`, valoarea `1.0` și timestampul
`0x0102030405060708`, octeții CRC de la final sunt `92 EF 69 7A`.

### Lecția 5: decodăm și verificăm

`decode` citește antetul, lungimea și CRC-ul, reconstruiește timestampul și
valoarea și validează numele. Construiește un `TsdbPoint` temporar; schimbă
`out` numai dacă întregul record este corect.

**Exemplu:** dacă schimbăm un bit al valorii la offset 16, antetul rămâne bun,
dar CRC-ul nu mai corespunde. Răspunsul este `TSDB_ERR_CHECKSUM`.

**Starea curentă a proiectului:** `encode` este terminat. În `decode` ai deja
verificarea pointerilor, a semnăturii, a versiunii, a lungimii și a octeților
rezervați. Urmează citirea CRC-ului, recalcularea lui, reconstrucția câmpurilor
și validarea rezultatului. Testele `round_trip` și `corruption` sunt încă roșii
din acest motiv.

**Criteriu de ieșire:** `record_tests.exe` trece; putem explica fiecare octet
și putem demonstra că o intrare coruptă nu modifică obiectul de ieșire.

## 3. Modulul II — Jurnalul de scriere și recuperarea (săptămânile 3–6)

**Întrebarea modulului:** ce se întâmplă dacă programul se oprește în timpul
scrierii?

### Lecția 6: append-only

Deschidem un fișier jurnal (WAL, „write-ahead log”) și adăugăm recorduri la
sfârșit. Nu rescriem punctele vechi în loc. Inițial, un jurnal simplu poate
conține recorduri de 92 de octeți unul după altul.

**Exemplu:** trei măsurători ocupă trei recorduri consecutive. Dacă ultima
scriere se oprește după 40 de octeți, la repornire citim primele două recorduri
complete și recunoaștem finalul incomplet.

### Lecția 7: citire la pornire și erori de disc

La `open`, parcurgem jurnalul, verificăm fiecare record și reconstruim starea
din memorie. Distingem un final tăiat de un record corupt aflat la mijloc:
primul se poate ignora conform regulii de recuperare; al doilea trebuie raportat
ca eroare, nu ascuns.

**Exemplu:** `bun | bun | 40 de octeți` se recuperează până la ultimul record
complet. `bun | CRC greșit | bun` este o corupere internă și deschiderea eșuează.

### Lecția 8: scrieri confirmate și batchuri atomice

`write_batch` va scrie mai multe puncte ca o singură operație logică. Pentru
asta, jurnalul va primi o încadrare a batchului și un marcaj de commit. La
recuperare, un batch fără marcaj complet nu devine vizibil.

**Exemplu:** batchul conține trei puncte; procesul cade după al doilea. După
repornire nu trebuie să vedem doar două puncte dintr-un batch care trebuia să
fie atomic. Vom testa oprirea în fiecare loc important.

Un răspuns de succes vine numai după sincronizarea jurnalului cu sistemul de
operare. Pe Windows vom învăța ce garantează `FlushFileBuffers`; simpla
întoarcere dintr-o funcție de scriere nu este aceeași promisiune. Chiar și așa,
nu pretindem protecție absolută față de orice defect al discului sau controlerului.

**Criteriu de ieșire:** un batch confirmat apare după repornire; un batch
neconfirmat nu apare parțial; erorile de scriere sunt raportate.

## 4. Modulul III — Index în memorie și reguli pentru duplicate (săptămânile 7–9)

**Întrebarea modulului:** cum găsim `cpu` fără să recitim fiecare punct `ram`?

### Lecția 9: organizăm punctele după metrică

Pornim cu o structură simplă: fiecare metrică are o colecție de puncte.
Învățăm alocare dinamică, capacitate, realocare, ownership și eliberarea
memoriei. Limita de memorie este explicită; la eșec de alocare întoarcem eroare.

**Exemplu:** `cpu` are 100 de puncte, `ram` are 100. O interogare pentru `cpu`
nu mai parcurge și cele 100 de puncte `ram`.

### Lecția 10: ordine temporală și date întârziate

Punctele pot sosi în afara ordinii: primim timestampurile 10, apoi 30, apoi 20.
Interogarea trebuie să le arate 10, 20, 30. Începem cu o soluție simplă și
măsurăm când trebuie un index mai eficient.

### Lecția 11: duplicate

Cheia logică a unui punct este `(metrică, timestamp)`. Dacă aceeași cheie se
scrie din nou, câștigă ultima **scriere confirmată**. Păstrăm o ordine internă
a commiturilor pentru a decide asta și după repornire.

**Exemplu:** `cpu@10 = 20`, apoi `cpu@10 = 25`; interogarea întoarce 25.

**Criteriu de ieșire:** indexul reconstruit din WAL dă același rezultat ca
indexul dinaintea închiderii, inclusiv pentru puncte întârziate și duplicate.

## 5. Modulul IV — Interogări și agregări (săptămânile 10–12)

### Lecția 12: intervale de timp

Implementarea folosește `query_range(metric, start, end)` cu interval
`[start, end]`: includem ambele limite. Pentru intervale alăturate trebuie
să alegem limite care nu dublează același timestamp.

**Exemplu:** punctele la 10, 20, 30; `[10, 30]` întoarce toate trei punctele.

### Lecția 13: agregări

Calculăm `count`, `min`, `max` și `avg` numai peste punctele din interval.
Definim explicit rezultatul pentru zero puncte: `count = 0`, iar celelalte
agregări sunt zero. Apelantul verifică `count` pentru a distinge un interval
gol de un interval care conține valoarea zero.

**Exemplu:** valorile 10, 20, 30 au `count=3`, `min=10`, `max=30`, `avg=20`.

### Lecția 14: CLI

CLI-ul primește comenzi precum `init`, `write`, `query`, `stats` și `check`.
Biblioteca rămâne separată: CLI-ul traduce textul utilizatorului în apeluri C.
Testăm și intrări greșite, de exemplu timestamp invalid sau interval inversat.

**Criteriu de ieșire:** interogările coincid cu un model simplu de referință,
inclusiv pentru metrici inexistente, intervale goale și valori duplicate.

## 6. Modulul V — Segmente imuabile pe disc (săptămânile 13–17)

**Întrebarea modulului:** cum evităm ca jurnalul și memoria să crească la
nesfârșit?

### Lecția 15: flush din memorie într-un segment

Scriem punctele vechi în fișiere noi, sortate după metrică și timestamp. Un
segment terminat nu se modifică. Păstrăm metadate precum intervalul de timp și
metricile conținute.

**Exemplu:** dacă un segment conține doar date din ianuarie, o interogare din
martie îl poate sări complet.

### Lecția 16: indexul segmentului

Adăugăm un index rar: la intervale regulate notăm poziția unor blocuri din
fișier. Căutăm mai întâi în index, apoi citim doar blocurile relevante.

**Exemplu:** pentru 1 milion de puncte, nu începem fiecare căutare la octetul
zero al fișierului.

### Lecția 17: stare persistentă și checkpoint

Un manifest indică segmentele active și locul sigur până la care datele din WAL
au fost mutate. Întâi scriem și verificăm noul segment, apoi schimbăm starea
activă. Nu ștergem jurnalul vechi înainte ca noua stare să fie recuperabilă.

**Exemplu:** procesul cade după ce segmentul a fost scris, dar înainte de
activarea lui. La repornire folosim manifestul anterior și WAL-ul; datele nu
dispar.

**Criteriu de ieșire:** interogările combină corect memoria și segmentele,
fără dubluri, iar un restart în timpul flushului nu pierde scrieri confirmate.

## 7. Modulul VI — Compactare și retenție (săptămânile 18–21)

### Lecția 18: compactare

Mai multe segmente mici devin unul mai mare. La îmbinare păstrăm ultima
versiune confirmată pentru aceeași metrică și același timestamp.

**Exemplu:** primul segment are `cpu@10=20`, al doilea are `cpu@10=25`;
segmentul compactat conține valoarea vizibilă 25.

### Lecția 19: schimbare sigură a segmentelor

Scriem fișiere noi separat, le verificăm, actualizăm manifestul și abia apoi
eliminăm fișierele vechi. Vom testa opriri între fiecare două operații. Pe
Windows, înlocuirea și sincronizarea fișierelor cer atenție specială.

### Lecția 20: retenție

O regulă precum „păstrează ultimele 30 de zile” calculează un prag temporal.
Punctele mai vechi dispar logic și sunt eliminate fizic la compactare. Nu
ștergem un întreg segment dacă el conține și date care trebuie păstrate.

**Criteriu de ieșire:** rezultatele sunt aceleași înainte și după compactare,
cu excepția punctelor expirate; spațiul vechi este recuperat în siguranță.

## 8. Modulul VII — Validare, măsurare și demo (săptămânile 22–26)

### Lecția 21: teste de crash

Un proces de test pornește baza, scrie puncte, îl oprește forțat în momente
stabilite și repornește baza. Comparăm ce a supraviețuit cu lista batchurilor
confirmate. Un test care doar închide frumos programul nu verifică recuperarea
după crash.

### Lecția 22: corupere și comparație cu un model

Modificăm o copie de test a unui fișier: tăiem ultimii octeți sau schimbăm un
octet intern. Folosim și un model simplu în memorie drept etalon pentru mii de
scrieri și interogări generate automat.

### Lecția 23: benchmark corect

Măsurăm puncte scrise pe secundă, timp de deschidere, timp de interogare și
spațiu pe disc. Notăm numărul de puncte, tipul discului, mărimea batchului și
dacă sincronizăm fiecare batch; altfel cifrele nu sunt comparabile.

### Lecția 24: date reale și prezentare

Un colector Windows simplu produce metrici CPU/RAM. Le scriem în baza noastră,
apoi demonstrăm interogare, medie, repornire și recuperare. README-ul final
conține arhitectura, comenzile, testele, limitele și rezultatele măsurate.

**Criteriu de ieșire v1:** putem demonstra date confirmate recuperate după
crash de proces, interogări corecte, compactare sigură și limite documentate.

## 9. Cum arată viitoarea interfață

Biblioteca va avea operații de forma:

```text
open(path) → bază sau eroare
write_batch(puncte[]) → confirmare sau eroare
query_range(metrică, start, end) → puncte ordonate
aggregate(metrică, start, end) → count/min/max/avg
close() → eliberare resurse
```

CLI-ul va apela această bibliotecă. Nu vom pune regulile bazei de date direct în
`main.c`; astfel putem testa motorul separat de felul în care utilizatorul scrie
comenzi în terminal.

## 10. Ce înseamnă „vandabil” după v1

Un motor generic ar concura cu baze de date mature. După ce v1 este fiabilă,
alegem o problemă precisă și discutăm cu utilizatori potențiali: de exemplu
metrici locale pe echipamente Windows care rămân offline sau auditul datelor
într-un laborator. Abia apoi decidem dacă merită un installer, dashboard,
serviciu sau suport plătit. Un benchmark bun nu dovedește singur că există
clienți.

## Surse de aprofundare, când ajungem la modulele respective

- Microsoft, `FlushFileBuffers`: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers
- InfluxDB OSS v2, arhitectura WAL/cache/segmente/compactare: https://docs.influxdata.com/influxdb/v2/reference/internals/storage-engine/
- CMake, manualul de utilizare: https://cmake.org/cmake/help/latest/guide/user-interaction/index.html

Nu trebuie să citești toate aceste documente acum. Le deschidem împreună când
conceptul apare în lecția curentă.
