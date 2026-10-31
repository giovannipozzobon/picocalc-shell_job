# Come la shell carica i programmi e il firmware Bluetooth

Tre spiegazioni per chi parte da zero: come la shell avvia un `.uf2` di
`/pico2-apps/`, di chi è il loader e se si può cambiare, e come il firmware
del chip Bluetooth viene letto dalla scheda SD invece di stare in RAM.
Il testo riassume `docs/design.md` (§3.1-3.4 e §3.10) e il codice in
`src/core/run.c`, `src/core/uf2.c`, `src/port/loader.c` e
`src/port/cyw43_fw.c`.

---

## 1. Come funziona il caricamento dei file `.uf2`

### Le basi

**Cos'è un file `.uf2`.** È un programma già compilato, impacchettato per
essere copiato su una scheda Raspberry Pi Pico. Dentro ci sono i byte da
scrivere e gli indirizzi dove metterli. È fatto di **blocchi da 512 byte**.
Ogni blocco ha:

- un'intestazione con tre numeri di riconoscimento fissi;
- 256 byte di programma;
- l'**indirizzo** in cui quei 256 byte vanno scritti;
- un **ID di famiglia**, che dice per quale chip è il programma. Per
  RP2350 sono accettati `0xe48bff59`, `0xe48bff5a` e `0xe48bff5b`.

**Dove va un programma.** La Pico ha due memorie:

- la **flash** (16 MB nella PicoCalc di prova): resta anche a scheda
  spenta, ed è dove di solito vive un programma. Si scrive lentamente e a
  settori da 4 KB, che prima vanno cancellati;
- la **RAM** (512 KB): è veloce ma si svuota allo spegnimento.

Dopo un riavvio la Pico esegue quello che trova nella flash. Per "avviare
un programma" bisogna quindi **scrivere il file nella flash e riavviare**.

**Chi aiuta: il loader.** Sul chip c'è già un piccolo programma, **UF2
Loader** (`stage3`), al primo posto in flash. Parte per primo a ogni
accensione. Decide cosa avviare: l'app in flash, il menu, o un file dalla
scheda SD.

**Dov'è la shell.** La shell non vive in flash: è un file sulla SD,
`/BOOT2350.UF2`, che il loader copia **in RAM** e avvia. Per questo può
riscrivere liberamente la flash dei programmi senza cancellare sé stessa.

### Cosa succede quando scrivi `pico-oric`

Il codice è in `src/core/run.c` (la logica) e `src/port/loader.c` (il
contatto col chip).

1. **Cerca il file.** Prima controlla se è un comando interno. Se no,
   cerca `pico-oric.uf2` in `/pico2-apps/`. Se scrivi un percorso con `/`,
   usa quello.
2. **Controlla che il loader sia quello giusto.** La shell sa avviare
   programmi solo con UF2 Loader 2.3, 2.4, 2.4.1, 2.5 e la versione 2.5
   modificata (`2.5-shell`). Legge la versione dal loader stesso. Se non
   va bene, rifiuta con un messaggio e **non tocca la flash**.
3. **Primo passaggio: controllare senza scrivere** (`uf2_check` in
   `src/core/uf2.c`). La shell legge tutto il file e verifica ogni blocco:
   - i numeri di riconoscimento devono essere giusti;
   - l'ID di famiglia deve essere di un RP2350 (i blocchi per altri chip
     vengono saltati);
   - gli indirizzi devono salire in ordine, senza buchi né ripetizioni;
   - il programma deve finire dentro la zona consentita: la shell non
     scrive mai fuori dalla partizione dei programmi.

   Un file sbagliato dà un errore (`not a UF2 program`, `not for this
   device`) e la flash resta com'era.
4. **Differenziale: scrive solo ciò che è cambiato.** Per ogni settore da
   4 KB la shell costruisce quello che dovrebbe contenere e lo confronta
   con quello che c'è già. Se è uguale, non lo scrive. Quindi rilanciare
   lo stesso programma costa solo una lettura del file, e la flash si
   consuma meno.
5. **Secondo passaggio: scrivere** (`uf2_write`), solo se qualche settore
   è diverso. Rilegge il file, cancella i settori diversi, li scrive a
   pagine da 256 byte e li rilegge per verifica. Sullo schermo vedi
   `writing 3 of 40 sectors`.
6. **Se la corrente salta mentre scrive.** Il primo settore del programma,
   quello che dice al chip "qui c'è un programma valido", viene cancellato
   per primo e riscritto **per ultimo**. Un'interruzione a metà lascia
   quindi un programma che il chip non riconosce come avviabile. Al
   riavvio il loader non trova nessuna app valida e carica
   `/BOOT2350.UF2`: torni alla shell. (Provato nei test host con
   interruzione a ogni operazione sulla flash.)
7. **Riavvia.** La shell scrive un comando in tre registri speciali del
   chip che sopravvivono a un riavvio (`watchdog scratch`) e fa un
   riavvio con il watchdog. All'avvio il loader non trova nessun tasto
   premuto né comandi pendenti e salta al programma appena scritto.

### Il caso particolare: i programmi in RAM

Alcuni `.uf2` (come `web.uf2`, `ntp.uf2`, `btpair.uf2`) hanno indirizzi
nella **RAM**, non nella flash. Non c'è niente da scrivere. La shell lo
vede dal controllo del passaggio 3 e passa al loader il comando
`BOOT_RAM` con il percorso del file. Il loader legge allora il file dalla
SD e lo copia in RAM da solo.

C'è una complicazione: il loader capisce solo i nomi corti 8.3
(`PICO-O~1.UF2`). La shell traduce quindi il percorso con `short_path()` e
mette la stringa in una zona di RAM che il riavvio non cancella.

Un programma RAM non tocca la flash: quello che c'era prima resta lì.

### Come si torna alla shell

- **Dal programma**: chiama `picocalc_shell_exit()` (nell'header
  `include/picocalc_shell.h`), che imposta il comando `BOOT_SD` e
  riavvia. Il loader carica la shell dalla SD invece dell'app. Funziona
  per i programmi in flash avviati dal loader. Quelli in RAM tornano
  scrivendo gli stessi registri a mano.
- **Dall'utente**: tenere premuto Su, F1 o F5 all'accensione.

### In una riga

La shell **controlla** il `.uf2`, **scrive in flash solo i settori
diversi** (o, per un programma RAM, passa al loader il percorso), e
**riavvia con un comando** per il loader: è il loader a far partire il
programma.

---

## 2. Il loader: di Raspberry o modificabile?

Il loader non è di Raspberry Pi, e si può cambiare. Ci sono due livelli
diversi.

**La ROM del chip (Raspberry Pi, non modificabile).** È dentro l'RP2350 e
non si può riscrivere. Fa partire il chip e legge la tabella delle
partizioni della flash. Offre anche la modalità **BOOTSEL**, in cui la
scheda appare come disco USB. Finché la ROM c'è, un loader sbagliato non
rende la scheda inutilizzabile: da BOOTSEL si riscrive la flash.

**UF2 Loader (di pelrun, modificabile).** È il programma in flash di cui
si parla sopra, scritto da pelrun (progetto UF2 Loader, versione 2.5) con
licenza **GPL-3.0**, come la shell. Il sorgente è nel repository come
sottomodulo in `third_party/uf2loader`. La parte che parte per prima,
`stage3`, occupa gli 8 KB all'inizio della flash. Il menu dei programmi,
che la shell ha sostituito, è un file sulla SD.

**È già stato modificato.** Con la milestone M7 la shell parte
all'accensione:

- la modifica è una patch in `patches/`;
- `tools/build-loader.sh` costruisce lo `stage3` con la patch, uno per
  ogni dimensione di flash (4, 8 o 16 MB); con `--stock` costruisce
  quello originale 2.5;
- la versione risultante si chiama `2.5-shell`, e la shell la riconosce.

**Come si installa.** Il file dello `stage3` va scritto all'inizio della
flash, di solito da BOOTSEL (o via SWD, come sulla Plus 2 W di prova). Si
fa una volta sola. Conviene salvare prima i primi 512 KB della flash.

**Da sapere.**

- La patch è facoltativa. La shell funziona anche con il loader originale
  2.5, partendo con un tasto premuto.
- Se si modifica `stage3` cambia il protocollo con la shell (registri
  scratch, comandi). Tutto ciò che lo riguarda sta in
  `src/port/loader.c`, quindi lì si adatta.
- Poiché è GPL-3.0, le modifiche vanno distribuite con la stessa licenza.

---

## 3. Il firmware del Bluetooth: dalla SD, non dalla RAM

Il firmware **non** viene caricato da RAM: sta su un file della scheda SD,
e la RAM è proprio ciò che questo meccanismo risparmia.

### Il problema

Il chip Bluetooth/Wi‑Fi della Pico 2 W (CYW43439) è un piccolo processore
separato. Quando si accende non ha nessun programma: il suo **firmware**
(circa 232 KB, di cui 231.077 B per il Wi‑Fi e 984 B per la configurazione
radio, detta CLM) va caricato ogni volta dalla Pico.

Il metodo normale del Pico SDK incorpora il firmware nel programma come un
grande array. Nella shell il programma sta tutto in RAM, quindi quell'array
occuperebbe 232 KB dei 512 KB disponibili. Non ci stava.

### La soluzione: il firmware sta sulla SD

Il firmware è un file, `/bluetooth/cyw43-fw.bin`, che la build produce
(`tools/cyw43-fw.py`) e che va copiato sulla scheda. La shell lo legge un
pezzo alla volta, mentre lo manda al chip, e non lo tiene mai tutto in
memoria.

### Come funziona

1. **Si cambia il driver (prima di compilare).** Il driver del chip
   (`cyw43_ll.c`, nell'SDK) usa l'array in tre punti: controlla la coda del
   firmware, lo scarica a blocchi, e legge il blocco CLM.
   `cmake/cyw43_fw.cmake` fa una copia del driver e la modifica al momento
   della configurazione. In quei tre punti, invece di leggere dall'array,
   chiama una funzione nostra, `cyw43_fw_read()`. Se il codice dell'SDK in
   quei punti cambia, la build si ferma con un messaggio. Gli altri
   programmi che usano il driver (come `ntp`) usano il codice originale.
2. **Apertura del file.** All'avvio della radio, `btkbd_init()` chiama
   `cyw43_fw_open()` (in `src/port/cyw43_fw.c`), che apre il file e ne
   controlla la lunghezza: se non c'è, dice `no firmware file on the
   card`; se la lunghezza non è quella giusta, `the firmware file is not
   this program's`. Così un file di un'altra build non viene mai mandato
   al chip.
3. **Lettura a pezzi.** Il driver chiede il firmware a blocchi da 64 byte,
   in ordine. Leggere 64 byte alla volta dalla SD sarebbe lentissimo,
   quindi `cyw43_fw_read()` tiene un **buffer da 4 KB**. Quando il driver
   chiede un dato che non c'è, la funzione legge 4 KB dal file in un colpo
   solo (FatFs li legge con un solo trasferimento) e poi serve i 64 byte
   dal buffer. Il costo in RAM è solo quei 4 KB.
4. **Quando parte il caricamento.** Il driver non carica il firmware
   quando si inizializza, ma alla **prima operazione sul chip**. Per
   questo `btkbd_init()` fa subito una piccola operazione (accende il LED
   collegato al chip) e il firmware viene scaricato in quel momento. Il
   file resta aperto fino a quando il chip è in funzione, poi viene
   chiuso.
5. **Un solo accesso alla SD per volta.** La lettura avviene nel ciclo
   principale della shell, non dentro BTstack. BTstack gira in background
   (in un interrupt) e, se leggesse la SD mentre il programma principale
   la usa, i due accessi si sovrapporrebbero. Per questo il caricamento è
   fatto prima che BTstack parta.

### Il risultato

| | Con array incorporato | Con file sulla SD |
|---|---:|---:|
| RAM per il firmware | circa 232 KB | 4 KB (buffer) |
| Radio pronta dopo l'avvio | — | circa 1 s |

Misurato sulla Pico 2 W: la radio è pronta in circa 1 secondo (1027 ms
nel test del proprietario, 2026-10-30).

### Cosa può andare storto

Se il file manca o è sbagliato, la shell parte lo stesso e stampa una
riga: `bluetooth: <motivo>`. Se non esiste nemmeno `pairing.dat`, la radio
non viene avviata e non si usa nemmeno il buffer.
