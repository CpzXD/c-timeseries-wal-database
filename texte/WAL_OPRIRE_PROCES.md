# Lecție: ce se întâmplă dacă programul este oprit brusc?

Am adăugat `tests/test_wal_crash.c`. El pornește un proces copil, îl oprește
forțat într-un moment controlat și redeschide baza în procesul părinte.
Build-ul Debug cu `/W4 /WX` a reușit, iar toate cele **9 teste CTest** trec.
Testul nou conține **8 scenarii**. Numărul testelor nu este numărul asserturilor.

## 1. De ce avem nevoie de acest pas?

Până acum verificam fișiere incomplete și erori de scriere simulate.
Mai trebuia să verificăm recuperarea atunci când dispare chiar programul
care ține baza deschisă. Dacă apelăm `tsdb_close`, programul are ocazia să
închidă normal resursele. În testul nou, copilul nu apelează această funcție:
părintele îl termină cu `TerminateProcess`.

Exemplu: programul scrie `cpu@1000 = 10`, primește succes, apoi este oprit.
La redeschidere, această valoare trebuie să existe.

## 2. Ce înseamnă proces părinte și proces copil?

Un proces este o instanță a unui program aflată în execuție. Pornim de două
ori același executabil, `wal_crash_tests.exe`:

```text
PĂRINTE: creează scenariul -> pornește copilul -> așteaptă semnalul
COPIL:   creează WAL-ul -> scrie -> semnalizează -> așteaptă
PĂRINTE: oprește copilul -> redeschide WAL-ul -> verifică rezultatul
```

Copilul este pornit cu argumentul `--child`. În `main`, această opțiune îl
trimite în `child_main`; fără argumente, executabilul rulează testele părintelui.
Fiecare proces are propria memorie. După oprirea copilului, părintele nu poate
folosi indexul din memoria acestuia: indexul trebuie reconstruit din fișier.

## 3. Ce date scriem?

Primul lot este confirmat întotdeauna:

| Metrică | Timestamp | Valoare |
|---|---:|---:|
| cpu | 1000 | 10 |
| ram | 2000 | 20 |

Al doilea lot conține:

| Metrică | Timestamp | Valoare | Ce verificăm |
|---|---:|---:|---|
| cpu | 1000 | 99 | Înlocuirea unei valori existente |
| cpu | 500 | 5 | Un punct sosit în afara ordinii |
| ram | 3000 | 30 | Un punct pentru altă metrică |

Dacă lotul al doilea este incomplet, după reparare trebuie să găsim doar
primul lot. Chiar dacă recordul `cpu@1000 = 99` este complet, el aparține
unui lot fără COMMIT complet. Valoarea veche, 10, trebuie să rămână vizibilă.
Nu aplicăm separat fiecare record întâlnit în coada incompletă.

Dacă lotul al doilea este complet și valid, interogarea CPU trebuie să dea
punctele `cpu@500 = 5`, apoi `cpu@1000 = 99`. Media lor este 52.
Pentru RAM avem 20 și 30, cu media 25.

## 4. Cum calculăm dimensiunile?

Folosim formatul WAL v2:

```text
Antet fișier: 16 octeți
BEGIN:        20 octeți
Un record:    92 octeți
COMMIT:       16 octeți
```

Primul lot are două puncte: `20 + 2 × 92 + 16 = 220` octeți.
Cu antetul fișierului: `16 + 220 = 236` octeți.
Al doilea lot are trei puncte: `20 + 3 × 92 + 16 = 312` octeți.
Fișierul cu ambele loturi complete are `236 + 312 = 548` octeți.

În cod, aceste formule sunt constantele `BASELINE_BATCH_SIZE`,
`PENDING_BATCH_SIZE`, `BASELINE_FILE_SIZE` și `MAX_FILE_SIZE`.
Nu numărăm octeții unei structuri C: folosim dimensiunile formatului pe disc.

## 5. Cum alegem exact momentul opririi?

Am adăugat `tsdb_wal_test_pause_write` în headerul intern
`src/wal_fault_hooks.h`. Funcția spune copiei de test a WAL-ului:

```text
La următoarea scriere, scrie N octeți reali în fișier,
apoi apelează funcția de pauză.
```

În `append_write_all`, ramura de test scrie prefixul cu `WriteFile`, apoi
apelează `test_checkpoint`. Restul lotului ar fi scris dacă funcția de pauză
s-ar întoarce. În aceste scenarii copilul rămâne în pauză până este terminat.
O pauză de 0 octeți înseamnă oprire înainte de scrierea următorului BEGIN.

`TsdbWalTestCheckpoint` este un **pointer la funcție**: în loc să stocăm un
număr, stocăm funcția care trebuie apelată la acel moment.
`void *context` transportă adresa handle-ului evenimentului. Funcția
`signal_and_wait` citește handle-ul și trimite semnalul către părinte.
Adresa rămâne validă deoarece variabila copilului există pe durata apelului.

Părintele și copilul comunică printr-un eveniment Windows. Copilul apelează
`SetEvent` abia după ce prefixul a fost scris. Părintele așteaptă evenimentul
sau ieșirea copilului cu `WaitForMultipleObjects`. Nu ghicim momentul opririi
printr-o întârziere de două secunde.

În scenariul după confirmare, copilul folosește API-ul public
`tsdb_write_batch`, așteaptă `TSDB_OK`, apoi trimite semnalul.

## 6. Cele opt scenarii

Numărul din coloana a doua este câți octeți există din al doilea lot.

| Momentul opririi | Octeți noi | Lungime WAL | Rezultat la deschidere |
|---|---:|---:|---|
| După succesul `tsdb_write_batch` | 312 | 548 | Ambele loturi |
| Înainte de următorul BEGIN | 0 | 236 | Doar primul lot |
| În timpul BEGIN | 7 | 243 | `TSDB_ERR_INCOMPLETE` |
| La jumătatea primului record | 66 | 302 | `TSDB_ERR_INCOMPLETE` |
| După primul record complet | 112 | 348 | `TSDB_ERR_INCOMPLETE` |
| Înainte de COMMIT | 296 | 532 | `TSDB_ERR_INCOMPLETE` |
| La jumătatea COMMIT | 304 | 540 | `TSDB_ERR_INCOMPLETE` |
| După COMMIT, înainte de flush și succes | 312 | 548 | Ambele loturi în acest test |

Pentru fiecare coadă incompletă verificăm că deschiderea nu modifică niciun
octet. Apoi `tsdb_repair` trebuie să creeze un backup identic cu fișierul
inițial și să elimine exact prefixul lotului al doilea. Comparăm efectiv
octeții backupului, nu doar lungimea lui. Prefixul confirmat trebuie să rămână
identic, iar interogările și agregările trebuie să redea valorile așteptate.

După recuperarea cu un singur lot, adăugăm din nou lotul al doilea prin API,
închidem și redeschidem. Astfel verificăm și că baza poate continua să scrie,
iar secvențele WAL rămân valide. Repararea unui fișier fără coadă este un
no-op: nu elimină octeți și nu creează backup.

## 7. COMMIT complet și confirmare către apelant

Acestea sunt două momente diferite. COMMIT este o structură din fișier.
Confirmarea către apelant înseamnă că funcția a terminat sincronizarea
cu `FlushFileBuffers` și a întors `TSDB_OK`.

Poate exista următoarea ordine:

```text
BEGIN + recorduri + COMMIT au fost scrise
programul este oprit înainte de flush și înainte să întoarcă succes
Windows continuă să ruleze
redeschiderea găsește lotul complet și valid
```

Recuperarea nu poate afla dacă apelantul a văzut succesul. Ea poate verifica
octeții, secvența și CRC-urile. De aceea un lot fără răspuns primit poate fi
vizibil integral la redeschidere. Un lot incomplet nu este aplicat parțial.
Lipsa confirmării nu înseamnă automat că datele lipsesc.

Testul oprește un proces și lasă Windows în funcțiune, inclusiv cache-ul
sistemului. El nu demonstrează recuperarea după o pană de curent. Scenariul
înainte de flush verifică tocmai această limită a confirmării.

## 8. Ce fac funcțiile Windows din test?

| Funcție sau constantă | Rol în test |
|---|---|
| `CreateProcessA` | Pornește propria copie a executabilului |
| `CREATE_NO_WINDOW` | Copilul nu deschide o fereastră de consolă |
| `CREATE_SUSPENDED` | Copilul așteaptă până configurăm gestionarea lui |
| `CreateJobObjectW` / `AssignProcessToJobObject` | Atașează copilul unui job |
| `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` | Oprește copilul dacă părintele dispare și handle-ul jobului se închide |
| `ResumeThread` | Permite copilului să înceapă după atașarea la job |
| `CreateEventA` / `OpenEventA` / `SetEvent` | Creează, deschide și semnalizează evenimentul de sincronizare |
| `TerminateProcess` | Oprește forțat procesul copil creat de test |
| `WaitForSingleObject` | Așteaptă ieșirea procesului înainte să citim fișierul |
| `GetExitCodeProcess` | Confirmă că oprirea a folosit codul stabilit de test |
| `CloseHandle` | Eliberează resursele Windows deținute de părinte |

Copilul are maximum 10 secunde să ajungă la pauză. CTest are și o limită
totală de 30 de secunde. Fiecare scenariu folosește un director temporar
separat. După succes este șters; după o eroare păstrăm fișierele și afișăm
directorul pentru diagnostic.

Macro-ul `CHECK` sare la eticheta `cleanup` când o verificare eșuează.
În aceste teste, această cale eliberează rezultatele interogărilor, închide
baza sau oprește copilul, după funcția în care ne aflăm. Scopul este ca o
verificare eșuată să nu lase un proces pornit sau un fișier blocat.

## 9. Ce fișiere am modificat?

- `tests/test_wal_crash.c`: orchestrarea proceselor și cele opt scenarii.
- `src/wal_fault_hooks.h`: tipul callbackului și funcția pentru pauză.
- `src/wal.c`: ramura de pauză și câmpurile necesare, sub
  `#ifdef TSDB_WAL_FAULT_TEST`.
- `CMakeLists.txt`: executabilul separat, testul `wal_process_crash` și timeoutul.

Biblioteca `tsdb_core` și CLI-ul sunt compilate fără această definiție.
Hookurile există numai în copiile de cod compilate pentru teste.
Fișierele C și headerele continuă să nu conțină comentarii.

## 10. Cum rulezi tu?

În terminalul PowerShell:

```powershell
Set-Location -LiteralPath 'C:\Users\username\Documents\baza de date in C'
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

`cmake -S . -B build` actualizează proiectele de compilare cu noul executabil.
`cmake --build` compilează sursele. `ctest` rulează executabilele deja compilate.
Rezultatul verificat este `100% tests passed out of 9`.

Pentru a vedea fiecare scenariu:

```powershell
.\build\Debug\wal_crash_tests.exe
```

Pentru a rula numai testul nou prin CTest:

```powershell
ctest --test-dir build -C Debug -R '^wal_process_crash$' --output-on-failure -V
```

`-R` selectează numele testului; `-V` afișează inclusiv mesajele `PASS`.

## 11. Un exercițiu mic

Avem primul lot confirmat, apoi 112 octeți din al doilea lot.

1. Câți octeți are fișierul întreg?
2. Primul record nou este complet. De ce nu putem aplica valoarea 99?
3. După `repair`, ce valoare trebuie să aibă `cpu@1000`?
4. De ce nu este suficient să verificăm doar lungimea backupului?

Răspunsuri: 348; lipsește COMMIT-ul complet al lotului; 10; două fișiere de
aceeași lungime pot conține octeți diferiți.

## 12. Unde suntem în roadmap?

Avem formatul recordului, WAL-ul și interogările etapei 3. Am verificat acum
și recuperarea după terminarea forțată a procesului, o parte din validarea
prevăzută în etapa 6. Urmează etapa 4: formatul segmentelor imuabile pe disc.

Tot indexul actual se află încă în RAM, iar WAL-ul crește. Segmentele vor
permite citirea datelor vechi de pe disc. Înainte să activăm un segment,
trebuie să proiectăm manifestul, secvențele pentru duplicate și publicarea
sigură după crash. Testele noi nu încheie validarea întregului motor:
compactarea, retenția, erorile de alocare și erorile din backup mai cer lucru.
