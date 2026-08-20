VSEBINA MAPE measurements
=========================

Mapa vsebuje izvožene merilne seje iz notranjega pomnilnika (LittleFS) ESP32.
Obe datoteki uporabljata isto glavo CSV, kot jo zapisuje program:

  zapis,cas_ms,cas_min,temperatura_C,zracna_vlaga_pct,svetloba_lux,
  zemlja_pct,zemlja_raw,temp_ok,vlaga_ok,svetloba_ok,zemlja_ok


-----------------------------------------------------------------------
1) meritve_24h.csv  -  REDNA 24-URNA MERITEV (rezultati v članku)
-----------------------------------------------------------------------
288 zapisov na 5 minut = 24 ur, ob spatifilu na polici ob severozahodnem
oknu (16. 8. 2026 10.50 -> 17. 8. 2026 10.50). Vse meritve veljavne.
Iz te seje so izdelani grafi v porocilo/slike/ in statistika v članku.

V tej seji se ni sprožilo nobeno opozorilo: razmere so bile večino časa
znotraj priporočenih območij, najdaljše temperaturno odstopanje pa je
trajalo 10 minut in ni doseglo enournega okna.


-----------------------------------------------------------------------
2) meritve_014.csv  -  MERITEV ZA DOKAZ DELOVANJA LOGIKE OPOZORIL
-----------------------------------------------------------------------
48 zapisov na 5 minut = 4 ure (seja 014 iz pomnilnika naprave).

Namen: 24-urna meritev poteka v običajnih bivalnih razmerah, zato v njej
opozorila ostanejo neaktivna in mehanizem ostane nepreverjen. V tej seji
je bil prototip namerno postavljen v skrajne razmere (neposredno sonce,
vroč in suh zrak), da se pokaže, ali se opozorila res sprožijo takrat,
ko so izpolnjeni pogoji, in da se NE sprožijo, kadar niso.

Izmerjeni razponi:
    temperatura      27,5  -  47,1 °C     (povprečje 32,6 °C)
    zračna vlaga     15,0  -  33,0 % RH   (povprečje 24,3 % RH)
    osvetljenost    980,0  -  54612,5 lx  (povprečje 14785,2 lx)
    vlaga substrata  60,4  -  68,9 %      (povprečje 66,1 %)

Preverjeno proti mejam profila spatifila in časovnim oknom iz src/main.cpp:

  TEMPERATURA (18-27 °C, okno 12 zapisov = 1 h)
      vseh 48 zapisov zunaj območja
      -> opozorilo SPROŽENO pri zapisu 12 (t = 60 min)

  ZRAČNA VLAGA (najmanj 40 % RH, okno 36 zapisov = 3 h)
      vseh 48 zapisov pod spodnjo mejo
      -> opozorilo SPROŽENO pri zapisu 36 (t = 180 min)

  NEPOSREDNA SVETLOBA (nad 10760 lx, okno 12 zapisov = 1 h)
      najdaljši neprekinjen niz nad pragom: 11 zapisov (55 min);
      pri zapisu 12 je vrednost padla na 8693,3 lx in niz se je prekinil
      -> opozorilo NI bilo sproženo

  VLAGA SUBSTRATA
      profil spatifila zanjo nima številčne meje (samo opisna kategorija
      "vlažen, ne moker"), zato se opozorilo ne presoja.

Zadnji primer je za dokazovanje enako pomemben kot prva dva: pokaže, da
se opozorilo ne sproži prezgodaj, tudi kadar je vrednost zelo visoka
(do 54612 lx), če pogoj o trajanju ni izpolnjen. Niz je bil prekratek
za en sam zapis, torej za 5 minut.

Opomba k prikazu: program na zaslonu OLED prikaže eno samo, najpomembnejše
opozorilo po prednostnem vrstnem redu. V tej seji sta bili hkrati aktivni
opozorili za temperaturo in zračno vlago, na zaslonu pa je bilo prikazano
opozorilo za temperaturo, ker je v vrstnem redu višje.


-----------------------------------------------------------------------
3) izvoz_p003_original.txt
-----------------------------------------------------------------------
Surov izpis seje 003 iz Serial Monitorja (ukaz P003), z oznakama
"--- ZACETEK CSV ---" in "--- KONEC CSV ---". Iz njega je nastal
meritve_24h.csv.
