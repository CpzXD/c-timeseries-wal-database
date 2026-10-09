# Lecție: ce se întâmplă când scrierea WAL eșuează?

## 1. Regula de bază

`tsdb_wal_write_batch` construiește lotul în memorie, apoi face trei lucruri:

```text
poziționează la sfârșit -> scrie BEGIN + recorduri + COMMIT -> FlushFileBuffers
```

Un fișier v2 cu un singur punct confirmat are 144 de octeți:
antetul fișierului (16) + BEGIN (20) + record (92) + COMMIT (16).
`FlushFileBuffers` este cererea către Windows de a sincroniza scrierea cu
dispozitivul. Succesul funcției este raportat abia după acest pas.

## 2. Exemplu: scrierea se oprește după 7 octeți

Avem deja un lot confirmat. În timpul lotului următor, doar 7 octeți ajung în
fișier, apoi scrierea raportează eroare. Funcția încearcă să readucă fișierul
la lungimea dinaintea lotului. Dacă reușește, rămân cei 144 de octeți inițiali.
Numărul de secvență 2 poate fi folosit la o redeschidere și o nouă scriere.

Dacă și această anulare eșuează, fișierul are 151 de octeți. Deschiderea
raportează `TSDB_ERR_INCOMPLETE` și **nu șterge nimic**. `repair` salvează
întregul fișier într-un backup nou înainte de a elimina cei 7 octeți.

## 3. Exemplu: lotul a fost scris, dar sincronizarea eșuează

Lotul poate fi complet în fișier, dar `FlushFileBuffers` raportează eroare.
Apelantul primește `TSDB_ERR_IO`, deci nu poate presupune că lotul a fost
confirmat. Funcția încearcă anularea. Dacă reușește, rămâne doar lotul vechi.
Dacă anularea eșuează, o redeschidere în același proces de test poate găsi
lotul nou complet. De aceea **nu repetăm orbește** scrierea după o eroare I/O:
închidem, redeschidem și verificăm ce date sunt vizibile. Această verificare
nu dovedește că datele ar fi supraviețuit unei căderi de curent.

## 4. Cum testăm fără să stricăm discul?

`tests/test_wal_fault.c` compilează propria copie a codului WAL, cu
`TSDB_WAL_FAULT_TEST`. Doar acea copie are funcțiile din
`src/wal_fault_hooks.h`. Ele pot simula o scriere parțială, un flush eșuat
sau o anulare eșuată. Biblioteca normală și CLI-ul nu conțin aceste hookuri.

Testează scenariile cu:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Testele verifică atât codul de eroare, cât și lungimea fișierului și loturile
recitite după redeschidere. Simularea trece prin aceeași ramură de cod ca o
eroare raportată de Windows; nu reproduce o pană reală de curent sau toate
modurile în care poate ceda un disc.
