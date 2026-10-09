Proiectul verificat la 7 octombrie 2026 are un motor funcțional pentru puncte
time-series: poate crea un jurnal, confirma loturi pe disc, reconstrui datele
după redeschidere, căuta după metrică și timp și calcula agregări. Build-ul
MSVC cu `/W4 /WX` reușește, iar cele 9 teste CTest trec. Acest raport explică
implementarea actuală și motivele alegerilor; segmentele, compactarea și
retenția sunt încă etape viitoare.

**Traseul unui punct începe în CLI și se termină în jurnal și index.** Pentru
`tsdb write date.bin cpu 1000 42.5`, CLI-ul citește argumentele, construiește
un `TsdbPoint` și cheamă `tsdb_write_batch`. API-ul rezervă memoria necesară,
WAL-ul codifică și sincronizează lotul, apoi indexul primește punctul. Dacă
WAL-ul raportează eroare, punctul nu este adăugat în index. După redeschidere,
indexul este refăcut din loturile valide ale jurnalului.

```text
argumente CLI -> TsdbPoint -> rezervare memorie -> lot WAL -> flush -> index
fișier WAL -> validare loturi -> decodare puncte -> index -> interogare
```

**Fișierele au responsabilități diferite, ca să putem urmări o singură idee
odată.** Un header declară ce funcții și tipuri sunt disponibile, iar un
fișier `.c` conține implementarea. Compilatorul verifică fiecare fișier, apoi
linkerul le unește. Nu trebuie să punem totul într-un singur fișier pentru ca
funcțiile să se poată apela.

| Fișier sau grup | Responsabilitate | Motivul separării |
|---|---|---|
| `include/tsdb/record.h` | Punctul, statusurile și encode/decode | Definește contractul datelor |
| `src/record.c` | Formatul recordului de 92 de octeți | Nu depinde de fișiere sau de CLI |
| `src/binary.h` | Numere little-endian de 32 și 64 de biți | Evită repetarea acelorași bucle |
| `src/crc32.c` | Calculul CRC incremental | Poate verifica un record sau un lot |
| `src/wal_format.c` | Antetul fișierului, BEGIN și COMMIT | Separă formatul de operațiile Windows |
| `src/wal.c` | Fișiere, loturi, flush, replay, repair | Gestionează persistența |
| `src/index.c` | Vectorul sortat din memorie | Gestionează ordinea și căutarea |
| `src/tsdb.c` | API-ul care unește WAL-ul și indexul | Coordonează scrierea și citirea |
| `src/main.c` | Comenzile CLI | Traduce textul în apeluri ale API-ului |
| `tests/` | Verificări automate | Detectează abateri de la contract |

**Bibliotecile standard ne oferă operațiile de bază.** Nu avem o bibliotecă
externă care implementează baza de date în locul nostru. `<stdint.h>` oferă
tipuri precum `uint8_t`, `uint32_t`, `uint64_t` și `int64_t`. `<stddef.h>`
oferă `size_t` și `NULL`. `<string.h>` oferă `memcpy`, `memmove`, `memcmp`,
`strcmp`, `strlen` și `memset`. `<stdlib.h>` oferă alocarea memoriei și
conversiile textului în numere. `<stdio.h>` oferă fișierele standard și
afișarea. `<math.h>` oferă verificarea valorilor finite. `<float.h>` descrie
reprezentarea numerelor reale. `<errno.h>` ajută la detectarea conversiilor
care depășesc limitele, iar `<inttypes.h>` oferă formate de afișare potrivite
pentru întregii de dimensiune fixă. `<windows.h>` oferă API-ul Windows folosit
pentru sincronizarea jurnalului.

**Un punct grupează trei informații.** `metric` identifică seria, de exemplu
`cpu`; `timestamp_ms` identifică momentul în milisecunde Unix; `value`
reprezintă măsurătoarea. Timestampul este semnat, deci acceptă și valori
negative. Numele poate avea între 1 și 63 de caractere. Array-ul are 64 de
poziții deoarece șirul C mai are nevoie de terminatorul `\0`. Acceptăm
litere ASCII, cifre, `_`, `-` și `.`. Nu acceptăm un nume gol, spații, NaN sau
infinit. Aceste limite țin reprezentarea și comparațiile previzibile.

**Recordul ocupă 92 de octeți, adică 736 de biți.** Structura C din memorie
nu este scrisă direct pe disc, deoarece compilatorul poate introduce padding,
iar ordinea octeților depinde de platformă. Noi construim explicit un array
de octeți cu poziții stabile.

| Poziții | Lungime | Conținut |
|---|---:|---|
| 0–3 | 4 | Semnătura `TSR1` |
| 4 | 1 | Versiunea recordului, 1 |
| 5 | 1 | Lungimea numelui metricii |
| 6–7 | 2 | Octeți rezervați, zero |
| 8–15 | 8 | Biții timestampului, little-endian |
| 16–23 | 8 | Biții valorii `double`, little-endian |
| 24–87 | 64 | Numele metricii și completare cu zero |
| 88–91 | 4 | CRC32 al octeților 0–87 |

Pentru `cpu`, octetul 5 este 3, iar octeții 24, 25 și 26 conțin `c`, `p`,
`u`. Octeții rămași ai câmpului sunt zero. În cod, pozițiile au nume precum
`RECORD_TIMESTAMP_OFFSET` și `RECORD_CRC_OFFSET`, ca să nu trebuiască să
ghicim ce reprezintă un număr precum 8 sau 88.

**Little-endian pune primul octetul cel mai puțin semnificativ.** Numărul
`0x12345678` devine `78 56 34 12`. `tsdb_put_u32_le` deplasează numărul cu
0, 8, 16 și 24 de biți și păstrează câte un octet. `tsdb_get_u32_le` face
operația inversă: deplasează fiecare octet la poziția lui și combină valorile
cu OR. Pentru 64 de biți se folosesc opt octeți. Aceste funcții sunt interne;
aplicația care folosește baza nu trebuie să le apeleze.

**`memcpy` copiază reprezentarea, fără conversie numerică.** Dacă am converti
numeric `42.5` în `uint64_t`, am obține un întreg, pierzând reprezentarea
valorii originale. Noi copiem cei opt octeți ai lui `double` într-un întreg,
apoi îi ordonăm pe disc. La decodare îi copiem înapoi într-un `double`.
Aceasta păstrează inclusiv diferența de biți dintre `0.0` și `-0.0`.
Verificările `_Static_assert` opresc compilarea pe o platformă care nu are
dimensiunea și parametrii binary64 ceruți de format.

**Encode validează înainte să modifice ieșirea.** Funcția verifică pointerii,
numele și valoarea, construiește recordul într-un array local, calculează CRC,
apoi copiază rezultatul în `out`. Un apel invalid lasă bufferul de ieșire
neschimbat. Decode urmează aceeași idee: verifică antetul și checksumul,
reconstruiește un punct temporar, validează numele, paddingul și valoarea,
apoi face `*out = decoded`. Astfel, o eroare nu lasă un punct reconstruit pe
jumătate în variabila apelantului.

**CRC32 este un calcul pentru detectarea modificărilor accidentale.** Are
32 de biți, deci ocupă patru octeți. Nu trebuie să aibă aceeași dimensiune ca
timestampul sau valoarea. Două mesaje diferite pot avea același CRC, iar
cineva care modifică intenționat fișierul poate recalcula checksumul.

Starea începe cu `0xFFFFFFFFu`. Pentru fiecare octet, algoritmul face XOR cu
starea, apoi procesează opt biți. `state & 1u` verifică bitul din dreapta.
Dacă acel bit este 1, starea este deplasată la dreapta și combinată prin XOR
cu `0xEDB88320u`. La final se aplică încă un XOR cu `0xFFFFFFFFu`. XOR nu
este adunare: `001 ^ 001` este `000`; nu există transport între poziții.

`start`, `update` și `finish` sunt separate pentru a putea calcula același
CRC pe bucăți. Putem chema `update` pentru BEGIN și apoi pentru recorduri,
fără să terminăm și să reîncepem calculul. Testul cu `123456789` verifică
rezultatul cunoscut `0xCBF43926`.

**WAL înseamnă Write-Ahead Log.** Datele sunt confirmate în jurnal înainte
ca indexul din memorie să fie actualizat. Memoria dispare când procesul se
oprește, dar jurnalul poate reconstrui indexul. Un lot este delimitat prin
BEGIN și COMMIT, astfel încât o scriere întreruptă să poată fi identificată.

| Componentă v2 | Dimensiune | Informații |
|---|---:|---|
| Antet de fișier | 16 | `TSDBWAL2`, câmpuri rezervate și CRC propriu |
| BEGIN | 20 | `TBG2`, count, sequence și CRC propriu |
| Record | 92 fiecare | Un punct cu CRC propriu |
| COMMIT | 16 | `TCM1`, sequence și CRC al BEGIN-ului și recordurilor |

Un lot cu un punct ocupă `20 + 92 + 16 = 128` de octeți. Primul lot dintr-un
fișier nou ocupă, împreună cu antetul, `16 + 128 = 144` de octeți. Două
loturi cu câte un punct ocupă 272 de octeți. `sequence` începe la 1 și crește
pentru fiecare lot confirmat; îl verificăm și în BEGIN, și în COMMIT.

**BEGIN v2 are CRC propriu pentru că trebuie să avem încredere în count
înainte să aflăm unde se termină lotul.** Formatul vechi v1 avea BEGIN de
16 octeți și nu putea verifica separat count. Dacă acel câmp se modifica,
parserul putea considera un lot complet drept incomplet. CRC-ul lotului nu
ajută înainte să știm unde este COMMIT-ul. Acesta este motivul pentru care
formatul v2 verifică antetul înainte să calculeze dimensiunea lotului.

**Scrierea unui lot validează toate punctele înainte de accesul la disc.**
WAL-ul alocă bufferul lotului, codifică BEGIN și recordurile, calculează CRC
și codifică COMMIT. Dacă un punct este invalid, bufferul este eliberat și
fișierul nu este scris. Limita de 4096 de puncte păstrează dimensiunea
alocărilor sub control.

După codificare, `SetFilePointerEx` poziționează fișierul la sfârșitul valid,
`WriteFile` scrie octeții și `FlushFileBuffers` cere sincronizarea cu
dispozitivul. Funcția raportează succes abia după flush. Buclele de citire și
scriere țin cont de posibilitatea ca o operație să transfere doar o parte
din date. Succesul sincronizării nu este o promisiune pentru orice defecțiune
posibilă a hardware-ului.

**După o eroare I/O, obiectul este marcat ca eșuat.** Nu continuăm scrierile
pe un handle cu rezultat incert. Codul încearcă anularea lotului curent,
revenind la lungimea de dinaintea scrierii. Dacă și anularea eșuează, pot
rămâne octeți incompleți sau un lot complet care nu a fost confirmat pentru
apelant. Trebuie să închidem și să redeschidem baza înainte să decidem ce
facem. O scriere care a raportat eroare poate fi vizibilă la redeschidere;
nu avem o garanție că orice eroare înseamnă absența lotului.

**Open verifică fișierul și păstrează conținutul.** `CREATE_NEW` este folosit
doar de create și refuză suprascrierea unui fișier existent. Open folosește
`OPEN_EXISTING`, deci un nume greșit nu creează o bază nouă. Un writer are
acces exclusiv; mai mulți cititori pot deschide împreună un fișier numai
pentru citire. Codul nu oferă acces simultan al cititorilor cu un writer și
nu este sincronizat pentru apeluri concurente pe același obiect.

Scanarea verifică versiunea, BEGIN-ul, count, secvența, lungimea lotului,
COMMIT-ul, CRC-ul lotului și fiecare record. `validate_batch_commit` și
`replay_batch_points` separă două responsabilități care înainte erau într-un
bloc lung. Callback-ul primește punctele doar pe durata apelului; dacă vrea
să le păstreze, trebuie să le copieze.

**Repair este o operație explicită.** Pentru o coadă incompletă v2, copiază
întregul fișier într-un backup nou, sincronizează backupul, apoi trunchiază
originalul la sfârșitul ultimului lot valid. Dacă backupul există deja,
operația este refuzată. Dacă fișierul este complet, raportează zero octeți
eliminați și nu creează backup. Coruperea unui lot complet este raportată
ca eroare, fără trunchiere.

Exemplu: avem 144 de octeți confirmați și doar 7 octeți din lotul următor.
Open raportează coada incompletă și păstrează cei 151 de octeți. Repair face
un backup de 151 de octeți și lasă originalul la 144. Backupul este locul
unde rămân și cei 7 octeți eliminați din original.

În acest review am închis riscul v1 rămas: orice coadă incompletă v1 este
acum refuzată de repair. O modificare a count din al doilea lot poate ascunde
loturi valide ulterioare chiar dacă primul lot este corect. Nu putem deduce
în siguranță ce trebuie eliminat. Jurnalele v1 complete rămân citibile și
pot primi scrieri în același format; o migrare v1 spre v2 nu este implementată.

**Indexul este un vector dinamic sortat după metrică și timestamp.**
`items` indică memoria, `count` spune câte puncte sunt folosite, iar
`capacity` spune câte încap în alocarea actuală. Capacitatea crește de obicei
prin dublare, ca să nu realocăm pentru fiecare punct. Verificările cu
`SIZE_MAX` împiedică depășirea limitelor la adunare și înmulțire. Rezultatul
`realloc` este păstrat într-un pointer nou; dacă alocarea eșuează, memoria
veche rămâne disponibilă.

Căutarea binară găsește poziția unui punct în aproximativ `log2(n)` pași.
Inserarea poate muta multe elemente cu `memmove`; aici sursa și destinația
se suprapun, deci `memcpy` nu este potrivit. Dacă cheia metrică/timestamp
există, punctul este înlocuit. De exemplu, `cpu, 200, 20` urmat de
`cpu, 200, 25` produce o singură valoare vizibilă, 25. Același comportament
este reconstruit prin replay în ordinea loturilor și a punctelor din lot.

**Memoria pentru inserare se rezervă înainte de confirmarea WAL.** Dacă am
confirma întâi scrierea, iar apoi alocarea pentru index ar eșua, am avea date
pe disc care nu sunt vizibile în indexul curent. Rezervarea înaintea scrierii
face ca inserarea de după flush să nu mai ceară o alocare nouă. În cazul
unei intrări invalide poate rămâne o capacitate suplimentară alocată, dar
datele vizibile și jurnalul rămân neschimbate.

**Interogarea include ambele limite ale intervalului.** Pentru `[10, 30]`,
punctele la 10, 20 și 30 sunt incluse. `[30, 30]` caută exact momentul 30.
`lower_bound` găsește primul punct cel puțin egal cu începutul, iar
`upper_bound` găsește primul punct strict mai mare decât sfârșitul. Diferența
dintre poziții este numărul de rezultate. API-ul copiază rezultatele într-o
alocare separată, iar apelantul le eliberează cu `tsdb_free_points`.

**Agregarea parcurge doar punctele din interval.** Count crește pentru fiecare
punct vizibil, min și max păstrează extremele, iar media este actualizată
progresiv. Formula împarte înainte de a combina valorile pentru a evita
overflow-ul unui total mare. Pentru un interval gol, toate câmpurile sunt
zero; apelantul verifică count înainte să interpreteze min/max/avg. Media
folosește aritmetică floating-point, deci nu promitem rezultate zecimale
perfect exacte pentru orice set de valori.

**CLI-ul are funcții mici pentru fiecare comandă.** `run_init`, `run_repair`,
`run_write` și `run_read` descriu acțiunea. Main verifică forma comenzii și
o trimite către funcția potrivită. `strtoll` și `strtod` citesc numerele;
verificăm dacă au consumat tot argumentul și dacă a apărut `ERANGE`.
`close_after_operation` păstrează tratarea erorilor de închidere într-un loc.
Interogările folosesc deschiderea readonly. `%.17g` păstrează suficientă
precizie pentru afișarea valorilor double.

**Codul este acum formatat consecvent și nu conține comentarii.** Explicațiile
și contractele sunt în documente. Numele funcțiilor, variabilelor și
constantelor poartă informația necesară citirii codului. `.clang-format`
fixează stilul pentru editările următoare. Testul WAL vechi a fost împărțit
în funcții pentru round-trip, record incomplet, BEGIN, COMMIT, CRC și lot.

**Testele oferă dovezi pentru scenarii concrete.** Numărul 9 reprezintă
executabilele sau comenzile înregistrate în CTest, nu numărul tuturor
asserturilor din proiect.

| Test CTest | Ce verifică |
|---|---|
| `setup_smoke` | Pornirea CLI și versiunea |
| `record_format` | Layout exact, biți double, CRC, intrări invalide și limita numelui |
| `wal_file_smoke` | Fișiere binare simple, coadă scurtă, vectori BEGIN/COMMIT/CRC |
| `wal_engine` | Loturi, secvențe, replay, backup și repair v2, corupere |
| `wal_safety` | Lipsa fișierului, readonly, v2 deteriorat, v1 compatibil și refuz repair |
| `wal_faults` | Scriere și flush eșuate, anulare reușită sau eșuată |
| `wal_process_crash` | Opt opriri forțate de proces, loturi confirmate, cozi incomplete, backup și reluarea scrierii |
| `query_index` | Date întârziate, duplicate, intervale și agregări |
| `query_model` | Comparație cu tabel independent, intervale variate și limite int64 |

Testele de fault sunt compilate separat cu `TSDB_WAL_FAULT_TEST`; biblioteca
normală și CLI-ul nu conțin hookurile de simulare. Modelul de referință
folosește un tabel metrică/timestamp și păstrează direct ultima valoare.
Acest tabel nu folosește căutarea binară a motorului. Comparăm rezultatele
înainte și după redeschidere, inclusiv timestampuri `INT64_MIN` și `INT64_MAX`.

**Recuperarea după oprirea procesului este acum testată.** Un proces copil
este terminat după succesul `tsdb_write_batch` și la șapte puncte controlate
din scrierea următoare. Verificăm lotul confirmat, integritatea backupului,
refuzul modificării la deschidere și continuarea scrierii după recuperare.
Inclusiv un record complet dintr-un lot incomplet este eliminat cu întregul
lot. Un COMMIT complet înainte de flush poate fi găsit la redeschidere,
deoarece Windows continuă să ruleze după terminarea procesului. Detaliile
sunt în `WAL_OPRIRE_PROCES.md`.

**Limitele rămase influențează ce putem promite.** Nu avem un test cu o
pană fizică de curent. Erorile de alocare și cele
din scrierea/sincronizarea backupului nu sunt injectate sistematic încă.

Tot indexul este în memorie, iar jurnalul crește continuu. Inserările în
ordine nefavorabilă pot face reconstruirea costisitoare deoarece vectorul
mută multe elemente. Deschiderea validează jurnalul și apoi îl parcurge
din nou pentru replay; este o alegere simplă, dar produce două parcurgeri.
Query copiază întregul rezultat, deci un rezultat mare mai cere memorie.

Windows este platforma actuală. `CreateFileA` folosește căi înguste; suportul
general pentru nume de fișiere Unicode nu este rezolvat. `TSDB_ERR_IO` grupează
mai multe erori Windows, deci diagnosticarea poate fi îmbunătățită. Dacă
alocarea obiectului WAL eșuează după crearea și sincronizarea antetului,
create poate lăsa un fișier valid pe disc deși raportează lipsă de memorie.
Acesta nu pierde date confirmate, dar poate surprinde la reîncercare.

**Ordinea recomandată pentru continuare este clară.** După verificarea
recuperării după terminarea forțată putem începe formatul segmentului pe disc,
cu secvența de scriere păstrată pentru rezolvarea duplicatelor. Segmentele
vor cere un manifest, citiri care combină memoria cu discul și teste pentru
oprire în timpul publicării. Compactarea și retenția vin după ce publicarea
segmentelor este sigură.

Pentru studiu, citește întâi `record.c` și `binary.h`, apoi `crc32.c` și
`wal_format.c`, apoi scrierea și scanarea din `wal.c`. Continuă cu `index.c`,
`tsdb.c` și CLI-ul. La fiecare funcție explică intrarea, ieșirea, cine deține
memoria și ce se întâmplă la eroare. Înțelegerea acestor contracte este mai
utilă decât memorarea tuturor liniilor.
