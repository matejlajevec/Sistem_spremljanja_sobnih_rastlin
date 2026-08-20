// =====================================================================
//  SISTEM ZA CASOVNO SPREMLJANJE POGOJEV SOBNIH RASTLIN
//  Matej Lajevec - STeKam 2026
//
//  Sistem meri temperaturo in relativno vlago zraka (DHT11), osvetljenost
//  (BH1750) ter relativno vlago substrata (kapacitivni senzor). Izmerjene
//  vrednosti primerja s priporocenimi obmocji izbrane rastline.
//
//  Bistvo delovanja: opozorilo se ne sprozi ob trenutni vrednosti, ampak
//  sele, ko je zunaj priporocenega obmocja doloceno stevilo ZAPOREDNIH
//  petminutnih zapisov. Tako kratko nihanje (oblak, odprto okno) ne
//  povzroci opozorila.
//
//  Strojna oprema:
//    ESP32 DevKit v1, vsi moduli na 3,3 V
//    OLED SSD1306 (0x3C) in BH1750 (0x23) na I2C: SDA GPIO21, SCL GPIO22
//    DHT11 na GPIO4, kapacitivni senzor zemlje na GPIO34 (ADC1)
//    tipka med GPIO27 in GND (notranji pull-up)
//
//  Serijski ukazi (115200 Bd, glej preveriSerijskiUkaz):
//    L, P, P001..P999, R, R1..R3, BRISI, DEMO, TT/TV/TZ/TL/TD/TN/TX
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BH1750.h>
#include <DHT.h>
#include <math.h>
#include <LittleFS.h>


// =====================================================================
//  1. NASTAVITVE
// =====================================================================

// --- Pini ---
const int PIN_SDA    = 21;
const int PIN_SCL    = 22;
const int PIN_DHT    = 4;
const int PIN_ZEMLJA = 34;   // ADC1
const int PIN_TIPKA  = 27;   // tipka proti GND, notranji pull-up

// --- Zaslon OLED ---
const int ZASLON_SIRINA = 128;
const int ZASLON_VISINA = 64;
const int OLED_NASLOV   = 0x3C;

// --- Casovni intervali (namesto blokirnih delay() klicev) ---
const unsigned long INTERVAL_MERITVE_MS = 2000UL;      // osvezitev senzorjev in zaslona
const unsigned long INTERVAL_ZAPISA_MS  = 300000UL;    // shranjevanje zapisa: 5 minut

// --- Kalibracija senzorja zemlje ---
// Dvotockovna kalibracija konkretnega senzorja. Rezultat je relativna
// lestvica tega senzorja, ne absolutni volumetricni delez vode.
const int ADC_SUHO  = 2622;   // suha zemlja  ->   0 %
const int ADC_MOKRO = 1128;   // mokra zemlja -> 100 %
const int ADC_MIN_VELJAVEN = 100;   // pod to vrednostjo senzor steje za odklopljen

// --- Testni nacin ob zagonu ---
// Med delovanjem ga vklopimo oziroma izklopimo z dolgim pritiskom tipke.
const bool TEST_MODE = false;


// =====================================================================
//  2. PODATKOVNI TIPI
// =====================================================================

// Ena meritev = ena vrstica v dnevniku. Za vsak senzor je poleg vrednosti
// shranjena tudi zastavica veljavnosti, saj se opozorila racunajo samo iz
// veljavnih meritev.
struct Meritev
{
    unsigned long casMs;   // cas od zagona v milisekundah

    float temperatura;     // stopinje C (DHT11)
    bool  tempOK;

    float vlagaZrak;       // % RH (DHT11)
    bool  vlagaZrakOK;

    float svetloba;        // lx (BH1750)
    bool  svetlobaOK;

    float vlagaZemlja;     // % kalibrirane relativne lestvice
    int   surovaZemlja;    // surova vrednost ADC
    bool  zemljaOK;
};

// Priblizen razred svetlobe (University of Minnesota, pretvorjeno iz
// foot-candles v lux). BH1750 meri lux, kar NI enako PPFD, zato gre za
// prakticno razvrstitev prototipa in ne za dokazano mejo poskodbe listov.
enum RazredSvetlobe
{
    NIZKA_SVETLOBA,
    SREDNJA_SVETLOBA,
    VISOKA_SVETLOBA
};

// Zgornja meja srednje svetlobe je hkrati dogovorjena inzenirska meja za
// opozorilo o neposredni svetlobi.
const float MEJA_NEPOSREDNE_SVETLOBE_LX = 10760.0;

// Kako naj se substrat izsusi med zalivanji. To je opisna kategorija in ne
// stevilcni odstotek, zato se iz nje opozorilo ne racuna.
enum NacinZemlje
{
    ZEMLJA_VLAZNA_NE_MOKRA,
    ZEMLJA_SKORAJ_SUHA,
    ZEMLJA_SUHA
};

// Priporocene razmere za eno rastlino
struct ProfilRastline
{
    const char *ime;
    const char *kratkoIme;
    const char *latinskoIme;

    float temperaturaMin;
    float temperaturaMax;

    bool  preverjajZracnoVlago;
    float zracnaVlagaMin;
    float zracnaVlagaMax;

    RazredSvetlobe svetlobaMin;
    RazredSvetlobe svetlobaMax;

    bool  preverjajUreSvetlobe;
    float svetlobaUreMin;
    float svetlobaUreMax;

    // Delovno obmocje za celodnevno presojo svetlobe. Kadar zanj ni
    // utemeljenega vira, ostane preverjajDnevnoSvetlobo na false in
    // stevilk ne izmisljamo.
    bool  preverjajDnevnoSvetlobo;
    float dnevnaSvetlobaLuxMin;
    float dnevnaSvetlobaLuxMax;

    NacinZemlje nacinZemlje;

    const char *vir;
};

// Izid celodnevne presoje svetlobe
enum DnevnaOcenaSvetlobe
{
    DAN_NI_KONCAN,
    DAN_NI_PODATKA,
    DAN_PREMALO_SVETLOBE,
    DAN_USTREZNO,
    DAN_PREVEC_SVETLOBE,
    DAN_MESANO
};

// Sporocilo, ki ima trenutno najvisjo prednost na zaslonu
enum PrikazStanje
{
    PRIKAZ_NORMALEN,
    PRIKAZ_NAPAKA_DHT,
    PRIKAZ_NAPAKA_BH1750,
    PRIKAZ_NAPAKA_ZEMLJA,
    PRIKAZ_NEPOSREDNA_SVETLOBA,
    PRIKAZ_VLAGA_ZEMLJE,
    PRIKAZ_TEMPERATURA,
    PRIKAZ_VLAGA_ZRAKA,
    PRIKAZ_DNEVNA_SVETLOBA,
    PRIKAZ_POGOJI_USTREZNI,
    PRIKAZ_POGOJI_SPET_USTREZNI
};

// Rocno izbrana simulacija v testnem nacinu (samo za prikaz)
enum TestnaSimulacija
{
    TEST_NIC,
    TEST_NIZKA_TEMPERATURA,
    TEST_NIZKA_VLAGA_ZRAKA,
    TEST_SUHA_ZEMLJA,
    TEST_VISOKA_SVETLOBA,
    TEST_NIZKA_DNEVNA_SVETLOBA
};


// =====================================================================
//  3. PROFILI RASTLIN
// =====================================================================
// Priporocila razlicnih virov se med seboj nekoliko razlikujejo, zato so
// spodnje vrednosti razumen priblizek in ne absolutno tocna meja.

const ProfilRastline RASTLINE[] = {
    // ime, kratkoIme, latinskoIme,
    // T min/max, preverjajRH, RH min/max,
    // razred svetlobe min/max, preverjajUre, ure min/max,
    // preverjajDnevnoSvetlobo, dnevno lx min/max, nacin zemlje, vir
    {
        "Spatifil", "SPATIFIL", "Spathiphyllum spp.",
        18.0, 27.0,
        true, 40.0, 100.0,
        NIZKA_SVETLOBA, SREDNJA_SVETLOBA,
        false, 0.0, 0.0,
        true, 270.0, 1080.0,
        ZEMLJA_VLAZNA_NE_MOKRA,
        "Gardenia - Peace Lily; preverjeno z RHS"
    },
    {
        "Potos", "POTOS", "Epipremnum aureum",
        18.0, 30.0,
        true, 50.0, 70.0,
        NIZKA_SVETLOBA, SREDNJA_SVETLOBA,
        false, 0.0, 0.0,
        false, 0.0, 0.0,
        ZEMLJA_SKORAJ_SUHA,
        "RHS - Pothos; Clemson University - humidity"
    },
    {
        "Aloe vera", "ALOE", "Aloe vera",
        13.0, 27.0,
        false, 0.0, 0.0,
        VISOKA_SVETLOBA, VISOKA_SVETLOBA,
        true, 6.0, 8.0,
        false, 0.0, 0.0,
        ZEMLJA_SUHA,
        "Gardenia - Aloe vera; preverjeno z RHS"
    }
};

const int STEVILO_RASTLIN = sizeof(RASTLINE) / sizeof(RASTLINE[0]);


// =====================================================================
//  4. MEJE OPOZORIL
// =====================================================================

// Casovna okna v normalnem nacinu, izrazena v stevilu zaporednih veljavnih
// petminutnih zapisov zunaj priporocenega obmocja.
const int OKNO_TEMPERATURA  = 12;   // 1 ura
const int OKNO_SVETLOBA     = 12;   // 1 ura
const int OKNO_VLAGA_ZRAKA  = 36;   // 3 ure
const int OKNO_VLAGA_ZEMLJE = 72;   // 6 ur - pripravljeno, glej opombo nizje

// V testnem nacinu se namesto zapisov stejejo priblizno dvosekundni tiki,
// da je opozorilo vidno v nekaj sekundah namesto v urah.
const int TEST_OKNO_TEMPERATURA  = 5;    // ~10 s
const int TEST_OKNO_SVETLOBA     = 5;    // ~10 s
const int TEST_OKNO_VLAGA_ZRAKA  = 10;   // ~20 s
const int TEST_OKNO_VLAGA_ZEMLJE = 15;   // ~30 s

// Histereza prepreci preklapljanje opozorila ob majhnih nihanjih okoli meje.
// Med stetjem velja navadna meja, aktivno opozorilo pa se izklopi sele ob
// vrnitvi v obmocje za vec kot to vrednost. Vrednosti so inzenirska
// nastavitev prototipa.
const float HISTEREZA_TEMPERATURA  = 0.5;    // stopinje C
const float HISTEREZA_VLAGA_ZRAKA  = 2.0;    // odstotne tocke
const float HISTEREZA_SVETLOBA     = 500.0;  // lx
const float HISTEREZA_VLAGA_ZEMLJE = 2.0;    // odstotne tocke - pripravljeno

// Opomba k vlagi substrata: okno in histereza sta pripravljena, opozorilo pa
// se v obicajnem nacinu ne sprozi. Vrtnarski viri podajajo samo opisno
// priporocilo ("vlazen, ne moker"), izmerjeni odstotek pa je relativna
// lestvica tega senzorja, zato utemeljenega stevilcnega praga ni mogoce
// dolociti. Vrednosti ostajata za primer, ko bo prag dolocen z lastnimi
// primerjalnimi meritvami.

// Celodnevna presoja svetlobe: 288 petminutnih zapisov = 24 ur.
const int   MAX_ZAPISOV      = 288;
const float MEJA_OSVETLJENOSTI_LX = 10.0;   // pod tem zapis velja za nocnega

// Kako dolgo po umiritvi vseh opozoril prikazujemo "POGOJI SPET USTREZNI"
const unsigned long TRAJANJE_SPOROCILA_MS = 5000UL;

// Korak avtomatskega konferencnega prikaza
const unsigned long DEMO_KORAK_MS = 10000UL;


// =====================================================================
//  5. NAPRAVE IN GLOBALNO STANJE
// =====================================================================

Adafruit_SSD1306 zaslon(ZASLON_SIRINA, ZASLON_VISINA, &Wire, -1);
BH1750 senzorSvetlobe;
DHT senzorDHT(PIN_DHT, DHT11);

bool oledDeluje      = false;
bool bh1750Deluje    = false;
bool littlefsDeluje  = false;

// --- Zadnja izmerjena vrednost in stetje zapisov ---
Meritev trenutnaMeritev;
int steviloZapisov = 0;              // 0..MAX_ZAPISOV, za prikaz "N: x/288"
unsigned long steviloVsehZapisov = 0;

// --- CSV seja ---
const int CSV_MAX_SEJA = 999;
const char *CSV_GLAVA =
    "zapis,cas_ms,cas_min,temperatura_C,zracna_vlaga_pct,svetloba_lux,"
    "zemlja_pct,zemlja_raw,temp_ok,vlaga_ok,svetloba_ok,zemlja_ok";
char csvPot[32] = "";                // npr. "/meritve_001.csv", dolocena ob zagonu

// --- Izbrana rastlina ---
int izbranaRastlina = 0;             // indeks v polju RASTLINE

// --- Stanje opozoril ---
// Za vsak parameter stejemo zaporedne zapise zunaj obmocja in hranimo,
// ali je opozorilo ze aktivno. Ob menjavi rastline se vse ponastavi.
int  stevecTemperatura = 0;
bool aktivnoTemperatura = false;

int  stevecVlagaZraka = 0;
bool aktivnoVlagaZraka = false;

int  stevecNeposrednaSvetloba = 0;
bool aktivnoNeposrednaSvetloba = false;

// Vlaga substrata je v prednostnem vrstnem redu prikaza, opozorilo pa se
// zanjo ne sprozi: profili imajo zanjo samo opisno kategorijo in ne
// utemeljene stevilcne meje.
bool aktivnoVlagaZemlje = false;

unsigned long casSporocilaUstreznoDo = 0;

// --- Celodnevna presoja svetlobe ---
int dnevnoSteviloZapisov = 0;
int dnevnoOsvetljenih    = 0;
int dnevnoPodMejo        = 0;
int dnevnoVObmocju       = 0;
int dnevnoNadMejo        = 0;
DnevnaOcenaSvetlobe dnevnaOcena = DAN_NI_KONCAN;

// --- Tipka ---
const unsigned long TIPKA_DEBOUNCE_MS     = 50UL;
const unsigned long TIPKA_DOLG_PRITISK_MS = 2000UL;

bool tipkaSurovoPrejsnje = HIGH;
bool tipkaStabilno       = HIGH;
unsigned long tipkaCasSpremembe = 0;
unsigned long tipkaCasPritiska  = 0;
bool tipkaDrzimo = false;
bool tipkaDolgSprozen = false;

// --- Testni nacin ---
bool aktivenTestniNacin = TEST_MODE;
TestnaSimulacija testnaSimulacija = TEST_NIC;
int  testniStevec = 0;
bool testniAlarmAktiven = false;
bool avtoDemoAktiven = false;
unsigned long avtoDemoZacetek = 0;


// --- Napovedi funkcij, ki jih uporabimo pred njihovo definicijo ---
void izberiRastlino(int indeks);
void ponastaviOpozorilaInDan();
void osveziOled(const Meritev &z);
void izpisiOledTestniNacin();
void vklopiTestniNacin();
void izklopiTestniNacin();
void izberiTestnoSimulacijo(TestnaSimulacija nova);
void zazeniAvtoDemo();
const char *besediloTestneSimulacije(TestnaSimulacija simulacija);
const char *besediloAvtoDemo(unsigned long preteceniMs);
const char *besediloDnevneOcene(DnevnaOcenaSvetlobe ocena);


// =====================================================================
//  6. BRANJE SENZORJEV
// =====================================================================

// Povprecje 64 zaporednih meritev ADC zmanjsa sum kapacitivnega senzorja.
int preberiSurovoZemljo()
{
    const int steviloVzorcev = 64;
    uint32_t vsota = 0;

    for (int i = 0; i < steviloVzorcev; i++)
    {
        vsota += analogRead(PIN_ZEMLJA);
        delay(2);
    }

    return vsota / steviloVzorcev;
}

// Pretvori surovo vrednost ADC v odstotek na kalibrirani relativni lestvici
float izracunajVlagoZemlje(int surovaVrednost)
{
    float odstotek = 100.0f * (ADC_SUHO - surovaVrednost) / (ADC_SUHO - ADC_MOKRO);

    if (odstotek < 0.0f)
    {
        odstotek = 0.0f;
    }
    if (odstotek > 100.0f)
    {
        odstotek = 100.0f;
    }

    return odstotek;
}

// Prebere vse senzorje in za vsakega posebej doloci veljavnost meritve
Meritev preberiSenzorje()
{
    Meritev z;

    z.casMs = millis();

    // DHT11 ob napaki vrne NaN
    z.temperatura = senzorDHT.readTemperature();
    z.tempOK      = !isnan(z.temperatura);

    z.vlagaZrak   = senzorDHT.readHumidity();
    z.vlagaZrakOK = !isnan(z.vlagaZrak);

    // BH1750 ob napaki vrne negativno vrednost
    z.svetloba   = NAN;
    z.svetlobaOK = false;

    if (bh1750Deluje)
    {
        float lux = senzorSvetlobe.readLightLevel();

        if (lux >= 0)
        {
            z.svetloba   = lux;
            z.svetlobaOK = true;
        }
    }

    // Zelo nizka vrednost ADC pomeni, da senzor zemlje ni priklopljen
    z.surovaZemlja = preberiSurovoZemljo();

    if (z.surovaZemlja < ADC_MIN_VELJAVEN)
    {
        z.zemljaOK     = false;
        z.vlagaZemlja  = 0.0f;
    }
    else
    {
        z.zemljaOK     = true;
        z.vlagaZemlja  = izracunajVlagoZemlje(z.surovaZemlja);
    }

    return z;
}

// Izpis trenutnih vrednosti v Serial Monitor (na INTERVAL_MERITVE_MS)
void izpisiSerial(const Meritev &z)
{
    Serial.println();
    Serial.println("[TRENUTNO]");

    if (z.tempOK)
    {
        Serial.printf("Temperatura: %.1f C\n", z.temperatura);
    }
    else
    {
        Serial.println("Temperatura: NAPAKA");
    }

    if (z.vlagaZrakOK)
    {
        Serial.printf("Zracna vlaga: %.1f %%\n", z.vlagaZrak);
    }
    else
    {
        Serial.println("Zracna vlaga: NAPAKA");
    }

    if (z.svetlobaOK)
    {
        Serial.printf("Osvetljenost: %.1f lx\n", z.svetloba);
    }
    else
    {
        Serial.println("Osvetljenost: NAPAKA");
    }

    if (z.zemljaOK)
    {
        Serial.printf("Zemlja: %.1f %% | RAW: %d\n", z.vlagaZemlja, z.surovaZemlja);
    }
    else
    {
        Serial.printf("Zemlja: NAPAKA | RAW: %d\n", z.surovaZemlja);
    }
}


// =====================================================================
//  7. SHRANJEVANJE V CSV (LittleFS)
// =====================================================================

// Poisce prvo prosto ime /meritve_001.csv .. /meritve_999.csv.
// Obstojecih datotek nikoli ne prepise, zato vsak zagon dobi svojo sejo.
bool poisciProstoImeDatoteke(char *izhod, size_t dolzinaIzhoda)
{
    for (int stevilkaSeje = 1; stevilkaSeje <= CSV_MAX_SEJA; stevilkaSeje++)
    {
        snprintf(izhod, dolzinaIzhoda, "/meritve_%03d.csv", stevilkaSeje);

        if (!LittleFS.exists(izhod))
        {
            return true;
        }
    }

    return false;
}

// Ustvari datoteko seje in vanjo zapise glavo stolpcev
bool pripraviCsvDatoteko()
{
    File datoteka = LittleFS.open(csvPot, FILE_WRITE);

    if (!datoteka)
    {
        Serial.print("NAPAKA: datoteke ni mogoce ustvariti: ");
        Serial.println(csvPot);
        return false;
    }

    datoteka.println(CSV_GLAVA);
    datoteka.close();
    return true;
}

// Stevilsko vrednost zapise v niz, neveljavno meritev pa kot "nan"
void steviloVCsvNiz(char *izhod, size_t dolzinaIzhoda, float vrednost,
                    bool veljavno, int decimalke)
{
    if (veljavno)
    {
        snprintf(izhod, dolzinaIzhoda, "%.*f", decimalke, vrednost);
    }
    else
    {
        snprintf(izhod, dolzinaIzhoda, "nan");
    }
}

// Sestavi eno vrstico CSV (brez znaka za novo vrstico)
void sestaviCsvVrstico(char *vrstica, size_t dolzinaVrstice, const Meritev &z,
                       unsigned long zaporednaStevilka)
{
    float casMin = z.casMs / 60000.0f;

    char temperaturaNiz[12];
    char vlagaNiz[12];
    char svetlobaNiz[12];
    char zemljaNiz[12];

    steviloVCsvNiz(temperaturaNiz, sizeof(temperaturaNiz), z.temperatura, z.tempOK, 1);
    steviloVCsvNiz(vlagaNiz,       sizeof(vlagaNiz),       z.vlagaZrak,   z.vlagaZrakOK, 1);
    steviloVCsvNiz(svetlobaNiz,    sizeof(svetlobaNiz),    z.svetloba,    z.svetlobaOK, 1);
    steviloVCsvNiz(zemljaNiz,      sizeof(zemljaNiz),      z.vlagaZemlja, z.zemljaOK, 1);

    snprintf(
        vrstica, dolzinaVrstice,
        "%lu,%lu,%.3f,%s,%s,%s,%s,%d,%d,%d,%d,%d",
        zaporednaStevilka,
        z.casMs,
        casMin,
        temperaturaNiz,
        vlagaNiz,
        svetlobaNiz,
        zemljaNiz,
        z.surovaZemlja,
        z.tempOK ? 1 : 0,
        z.vlagaZrakOK ? 1 : 0,
        z.svetlobaOK ? 1 : 0,
        z.zemljaOK ? 1 : 0
    );
}

// Doda vrstico na konec datoteke seje. Datoteko je treba zapreti, da se
// podatki res zapisejo v Flash.
bool zapisiVrsticoVDatoteko(const char *vrstica)
{
    if (!littlefsDeluje)
    {
        return false;
    }

    File datoteka = LittleFS.open(csvPot, FILE_APPEND);

    if (!datoteka)
    {
        Serial.print("NAPAKA: datoteke ni mogoce odpreti za dodajanje: ");
        Serial.println(csvPot);
        return false;
    }

    datoteka.println(vrstica);
    datoteka.close();
    return true;
}

// Izpise celotno datoteko med oznakama, primerno za kopiranje iz terminala
void izpisiDatoteko(const char *pot)
{
    if (!littlefsDeluje)
    {
        Serial.println("NAPAKA: LittleFS ni na voljo");
        return;
    }

    if (!LittleFS.exists(pot))
    {
        Serial.print("NAPAKA: datoteka ne obstaja: ");
        Serial.println(pot);
        return;
    }

    File datoteka = LittleFS.open(pot, FILE_READ);

    if (!datoteka)
    {
        Serial.print("NAPAKA: datoteke ni mogoce odpreti: ");
        Serial.println(pot);
        return;
    }

    Serial.println("--- ZACETEK CSV ---");

    while (datoteka.available())
    {
        Serial.write(datoteka.read());
    }

    Serial.println();
    Serial.println("--- KONEC CSV ---");

    datoteka.close();
}

// Izpise vse seje z velikostjo v bajtih
void izpisiSeznamDatotek()
{
    if (!littlefsDeluje)
    {
        Serial.println("NAPAKA: LittleFS ni na voljo");
        return;
    }

    Serial.println("--- SEZNAM DATOTEK ---");

    File koren = LittleFS.open("/");
    File datoteka = koren.openNextFile();

    while (datoteka)
    {
        String ime = datoteka.name();

        if (ime.startsWith("meritve_") && ime.endsWith(".csv"))
        {
            Serial.print(ime);
            Serial.print("  ");
            Serial.print(datoteka.size());
            Serial.println(" B");
        }

        datoteka = koren.openNextFile();
    }

    Serial.println("--- KONEC SEZNAMA ---");
}

// Izbrise samo datoteke sej, drugih datotek se ne dotakne
int izbrisiVseSeje()
{
    int steviloIzbrisanih = 0;

    for (int stevilkaSeje = 1; stevilkaSeje <= CSV_MAX_SEJA; stevilkaSeje++)
    {
        char pot[32];
        snprintf(pot, sizeof(pot), "/meritve_%03d.csv", stevilkaSeje);

        if (LittleFS.exists(pot) && LittleFS.remove(pot))
        {
            steviloIzbrisanih++;
        }
    }

    return steviloIzbrisanih;
}


// =====================================================================
//  8. LOGIKA OPOZORIL
// =====================================================================

// Posodobi opozorilo za parameter z obojestransko mejo.
// Neveljavne meritve stevca ne spremenijo, saj bi sicer okvara senzorja
// sprozila ali izbrisala opozorilo.
void posodobiOpozorilo(bool veljavno, float vrednost,
                       float spodnjaMeja, float zgornjaMeja, float histereza,
                       int zahtevanoStevilo, int &stevec, bool &aktivno)
{
    if (!veljavno)
    {
        return;
    }

    if (!aktivno)
    {
        // Stejemo zaporedne zapise zunaj obmocja; vrnitev v obmocje niz prekine
        if (vrednost < spodnjaMeja || vrednost > zgornjaMeja)
        {
            stevec++;
            if (stevec >= zahtevanoStevilo)
            {
                aktivno = true;
            }
        }
        else
        {
            stevec = 0;
        }
    }
    else
    {
        // Za izklop zahtevamo vrnitev v obmocje z varnostnim pasom (histerezo)
        if (vrednost >= spodnjaMeja + histereza && vrednost <= zgornjaMeja - histereza)
        {
            aktivno = false;
            stevec = 0;
        }
    }
}

// Enako, a samo z zgornjo mejo (neposredna svetloba)
void posodobiOpozoriloZgornjaMeja(bool veljavno, float vrednost, float meja,
                                  float histereza, int zahtevanoStevilo,
                                  int &stevec, bool &aktivno)
{
    if (!veljavno)
    {
        return;
    }

    if (!aktivno)
    {
        if (vrednost > meja)
        {
            stevec++;
            if (stevec >= zahtevanoStevilo)
            {
                aktivno = true;
            }
        }
        else
        {
            stevec = 0;
        }
    }
    else
    {
        if (vrednost <= meja - histereza)
        {
            aktivno = false;
            stevec = 0;
        }
    }
}

bool katerokoliOpozoriloAktivno()
{
    return aktivnoTemperatura || aktivnoVlagaZraka
        || aktivnoNeposrednaSvetloba || aktivnoVlagaZemlje;
}

// Preveri opozorila ob enem na novo shranjenem petminutnem zapisu
void preveriOpozorila(const Meritev &z)
{
    const ProfilRastline &profil = RASTLINE[izbranaRastlina];
    bool bilaAktivna = katerokoliOpozoriloAktivno();

    posodobiOpozorilo(z.tempOK, z.temperatura,
                      profil.temperaturaMin, profil.temperaturaMax,
                      HISTEREZA_TEMPERATURA, OKNO_TEMPERATURA,
                      stevecTemperatura, aktivnoTemperatura);

    posodobiOpozoriloZgornjaMeja(z.svetlobaOK, z.svetloba,
                      MEJA_NEPOSREDNE_SVETLOBE_LX,
                      HISTEREZA_SVETLOBA, OKNO_SVETLOBA,
                      stevecNeposrednaSvetloba, aktivnoNeposrednaSvetloba);

    if (profil.preverjajZracnoVlago)
    {
        posodobiOpozorilo(z.vlagaZrakOK, z.vlagaZrak,
                          profil.zracnaVlagaMin, profil.zracnaVlagaMax,
                          HISTEREZA_VLAGA_ZRAKA, OKNO_VLAGA_ZRAKA,
                          stevecVlagaZraka, aktivnoVlagaZraka);
    }

    // Vlaga substrata: okno OKNO_VLAGA_ZEMLJE in HISTEREZA_VLAGA_ZEMLJE sta
    // pripravljena, klica pa namenoma ni - profil zanjo nima utemeljene
    // stevilcne meje, zato bi vsak prag bil izmisljen. Meritev se kljub temu
    // shranjuje v CSV.

    // Ko se zadnje opozorilo umiri, to za kratek cas sporocimo na zaslonu
    if (bilaAktivna && !katerokoliOpozoriloAktivno())
    {
        casSporocilaUstreznoDo = millis() + TRAJANJE_SPOROCILA_MS;
    }
}

// Stevci veljajo za en profil, zato jih ob menjavi rastline ponastavimo
void ponastaviOpozorilaInDan()
{
    stevecTemperatura = 0;
    aktivnoTemperatura = false;

    stevecVlagaZraka = 0;
    aktivnoVlagaZraka = false;

    stevecNeposrednaSvetloba = 0;
    aktivnoNeposrednaSvetloba = false;

    aktivnoVlagaZemlje = false;

    dnevnoSteviloZapisov = 0;
    dnevnoOsvetljenih    = 0;
    dnevnoPodMejo        = 0;
    dnevnoVObmocju       = 0;
    dnevnoNadMejo        = 0;
    dnevnaOcena          = DAN_NI_KONCAN;

    casSporocilaUstreznoDo = 0;
}


// =====================================================================
//  9. CELODNEVNA PRESOJA SVETLOBE
// =====================================================================

const char *besediloDnevneOcene(DnevnaOcenaSvetlobe ocena)
{
    if (ocena == DAN_PREMALO_SVETLOBE)
    {
        return "PREMALO SVETLOBE";
    }
    if (ocena == DAN_USTREZNO)
    {
        return "SVETLOBA USTREZNA";
    }
    if (ocena == DAN_PREVEC_SVETLOBE)
    {
        return "PREVEC SVETLOBE";
    }
    if (ocena == DAN_MESANO)
    {
        return "SVETLOBA MESANA";
    }
    return "SVETLOBA: NI PODATKA";
}

// Po 288 zapisih doloci oceno dneva in zacne novo 24-urno obdobje.
// Odloci prevladujoca skupina, torej tista z vec kot polovico zapisov.
void zakljuciDan()
{
    if (dnevnoOsvetljenih == 0)
    {
        dnevnaOcena = DAN_NI_PODATKA;
    }
    else
    {
        float delezPod = (float)dnevnoPodMejo  / dnevnoOsvetljenih;
        float delezV   = (float)dnevnoVObmocju / dnevnoOsvetljenih;
        float delezNad = (float)dnevnoNadMejo  / dnevnoOsvetljenih;

        if (delezPod > 0.5f)
        {
            dnevnaOcena = DAN_PREMALO_SVETLOBE;
        }
        else if (delezV > 0.5f)
        {
            dnevnaOcena = DAN_USTREZNO;
        }
        else if (delezNad > 0.5f)
        {
            dnevnaOcena = DAN_PREVEC_SVETLOBE;
        }
        else
        {
            dnevnaOcena = DAN_MESANO;
        }
    }

    Serial.print("[DAN] Zakljucena 24-urna presoja svetlobe: ");
    Serial.println(besediloDnevneOcene(dnevnaOcena));

    dnevnoSteviloZapisov = 0;
    dnevnoOsvetljenih    = 0;
    dnevnoPodMejo        = 0;
    dnevnoVObmocju       = 0;
    dnevnoNadMejo        = 0;
}

// V presojo stejejo samo veljavni zapisi nad MEJA_OSVETLJENOSTI_LX.
// Nocni zapisi se v CSV shranijo, v oceno dneva pa ne gredo.
void posodobiDnevnoSvetlobo(const Meritev &z)
{
    const ProfilRastline &profil = RASTLINE[izbranaRastlina];

    if (!profil.preverjajDnevnoSvetlobo)
    {
        dnevnaOcena = DAN_NI_PODATKA;
        return;
    }

    dnevnoSteviloZapisov++;

    if (z.svetlobaOK && z.svetloba > MEJA_OSVETLJENOSTI_LX)
    {
        dnevnoOsvetljenih++;

        if (z.svetloba < profil.dnevnaSvetlobaLuxMin)
        {
            dnevnoPodMejo++;
        }
        else if (z.svetloba > profil.dnevnaSvetlobaLuxMax)
        {
            dnevnoNadMejo++;
        }
        else
        {
            dnevnoVObmocju++;
        }
    }

    if (dnevnoSteviloZapisov >= MAX_ZAPISOV)
    {
        zakljuciDan();
    }
}


// =====================================================================
//  10. PRIKAZ NA ZASLONU OLED
// =====================================================================

// Hkrati je lahko aktivnih vec opozoril, prikazano pa je eno - prvo po
// tem vrstnem redu.
PrikazStanje ugotoviPrikazStanje(const Meritev &z)
{
    // Napaka senzorja ima prednost pred vsemi opozorili
    if (!z.tempOK || !z.vlagaZrakOK)
    {
        return PRIKAZ_NAPAKA_DHT;
    }
    if (!z.svetlobaOK)
    {
        return PRIKAZ_NAPAKA_BH1750;
    }
    if (!z.zemljaOK)
    {
        return PRIKAZ_NAPAKA_ZEMLJA;
    }

    if (millis() < casSporocilaUstreznoDo)
    {
        return PRIKAZ_POGOJI_SPET_USTREZNI;
    }

    if (aktivnoNeposrednaSvetloba)
    {
        return PRIKAZ_NEPOSREDNA_SVETLOBA;
    }
    if (aktivnoVlagaZemlje)
    {
        return PRIKAZ_VLAGA_ZEMLJE;
    }
    if (aktivnoTemperatura)
    {
        return PRIKAZ_TEMPERATURA;
    }
    if (aktivnoVlagaZraka)
    {
        return PRIKAZ_VLAGA_ZRAKA;
    }

    // Ocena dneva se prikaze sele, ko je 24-urno obdobje zakljuceno
    if (dnevnaOcena == DAN_PREMALO_SVETLOBE || dnevnaOcena == DAN_PREVEC_SVETLOBE ||
        dnevnaOcena == DAN_MESANO || dnevnaOcena == DAN_NI_PODATKA)
    {
        return PRIKAZ_DNEVNA_SVETLOBA;
    }
    if (dnevnaOcena == DAN_USTREZNO)
    {
        return PRIKAZ_POGOJI_USTREZNI;
    }

    return PRIKAZ_NORMALEN;
}

// Prikaz enega sporocila namesto obicajnih meritev.
// Zaslon nima sumnikov, zato so sporocila brez njih in z velikimi crkami.
void izpisiSporociloNaOled(const char *vrstica1, const char *vrstica2)
{
    zaslon.clearDisplay();
    zaslon.setTextColor(SSD1306_WHITE);
    zaslon.setTextSize(1);

    zaslon.setCursor(0, 0);
    zaslon.print(RASTLINE[izbranaRastlina].kratkoIme);
    zaslon.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    zaslon.setCursor(0, 25);
    zaslon.println(vrstica1);

    zaslon.setCursor(0, 40);
    zaslon.println(vrstica2);

    zaslon.display();
}

// Obicajni prikaz: ime rastline, stevec zapisov in vse stiri meritve.
// Surova vrednost ADC je namenoma samo v Serial Monitorju in CSV.
void osveziOled(const Meritev &z)
{
    if (!oledDeluje)
    {
        return;
    }

    if (aktivenTestniNacin)
    {
        izpisiOledTestniNacin();
        return;
    }

    switch (ugotoviPrikazStanje(z))
    {
        case PRIKAZ_NAPAKA_DHT:
            izpisiSporociloNaOled("NAPAKA DHT11", "");
            return;
        case PRIKAZ_NAPAKA_BH1750:
            izpisiSporociloNaOled("NAPAKA BH1750", "");
            return;
        case PRIKAZ_NAPAKA_ZEMLJA:
            izpisiSporociloNaOled("NAPAKA ZEMLJA", "");
            return;
        case PRIKAZ_POGOJI_SPET_USTREZNI:
            izpisiSporociloNaOled("", "POGOJI SPET USTREZNI");
            return;
        case PRIKAZ_NEPOSREDNA_SVETLOBA:
            izpisiSporociloNaOled("OPOZORILO:", "NEPOSREDNA SVETLOBA");
            return;
        case PRIKAZ_VLAGA_ZEMLJE:
            izpisiSporociloNaOled("OPOZORILO:", "VLAGA ZEMLJE");
            return;
        case PRIKAZ_TEMPERATURA:
            izpisiSporociloNaOled("OPOZORILO:", "TEMPERATURA");
            return;
        case PRIKAZ_VLAGA_ZRAKA:
            izpisiSporociloNaOled("OPOZORILO:", "ZRACNA VLAGA");
            return;
        case PRIKAZ_DNEVNA_SVETLOBA:
            izpisiSporociloNaOled("DNEVNA SVETLOBA:", besediloDnevneOcene(dnevnaOcena));
            return;
        case PRIKAZ_POGOJI_USTREZNI:
            izpisiSporociloNaOled("", "POGOJI USTREZNI");
            return;
        default:
            break;   // PRIKAZ_NORMALEN - nadaljujemo z obicajnim prikazom
    }

    zaslon.clearDisplay();
    zaslon.setTextColor(SSD1306_WHITE);
    zaslon.setTextSize(1);

    // 1. vrstica: rastlina in stevec shranjenih zapisov
    zaslon.setCursor(0, 0);
    zaslon.print(RASTLINE[izbranaRastlina].kratkoIme);

    zaslon.setCursor(70, 0);
    zaslon.print("N:");
    zaslon.print(steviloZapisov);
    zaslon.print("/");
    zaslon.print(MAX_ZAPISOV);

    zaslon.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    // 2. vrstica: temperatura in zracna vlaga
    zaslon.setCursor(0, 13);
    zaslon.print("T:");
    if (z.tempOK)
    {
        zaslon.print(z.temperatura, 1);
        zaslon.print("C");
    }
    else
    {
        zaslon.print("ERR");
    }

    zaslon.setCursor(64, 13);
    zaslon.print("RH:");
    if (z.vlagaZrakOK)
    {
        zaslon.print(z.vlagaZrak, 0);
        zaslon.print("%");
    }
    else
    {
        zaslon.print("ERR");
    }

    // 3. vrstica: osvetljenost
    zaslon.setCursor(0, 23);
    zaslon.print("L:");
    if (z.svetlobaOK)
    {
        zaslon.print(z.svetloba, 0);
        zaslon.print(" lx");
    }
    else
    {
        zaslon.print("ERR");
    }

    // 4. vrstica: vlaga substrata
    zaslon.setCursor(0, 33);
    zaslon.print("S:");
    if (z.zemljaOK)
    {
        zaslon.print(z.vlagaZemlja, 0);
        zaslon.print("%");
    }
    else
    {
        zaslon.print("ERR");
    }

    zaslon.display();
}


// =====================================================================
//  11. IZBIRA RASTLINE
// =====================================================================

const char *besediloSvetlobe(RazredSvetlobe razred)
{
    if (razred == NIZKA_SVETLOBA)
    {
        return "NIZKA";
    }
    if (razred == SREDNJA_SVETLOBA)
    {
        return "SREDNJA";
    }
    return "VISOKA";
}

const char *besediloZemlje(NacinZemlje nacin)
{
    if (nacin == ZEMLJA_VLAZNA_NE_MOKRA)
    {
        return "VLAZNA, NE MOKRA";
    }
    if (nacin == ZEMLJA_SKORAJ_SUHA)
    {
        return "SKORAJ SUHA PRED ZALIVANJEM";
    }
    return "SUHA PRED ZALIVANJEM";
}

void izpisiSeznamRastlin()
{
    Serial.println("--- RASTLINE ---");

    for (int i = 0; i < STEVILO_RASTLIN; i++)
    {
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(RASTLINE[i].ime);

        if (i == izbranaRastlina)
        {
            Serial.print(" [IZBRANA]");
        }

        Serial.println();
    }

    Serial.println("--- KONEC SEZNAMA ---");
}

void izpisiProfilRastline(const ProfilRastline &profil)
{
    Serial.print("[RASTLINA] Izbrana: ");
    Serial.println(profil.ime);

    Serial.print("Latinsko ime: ");
    Serial.println(profil.latinskoIme);

    Serial.print("Temperatura: ");
    Serial.print(profil.temperaturaMin, 1);
    Serial.print("-");
    Serial.print(profil.temperaturaMax, 1);
    Serial.println(" C");

    Serial.print("Zracna vlaga: ");
    if (profil.preverjajZracnoVlago)
    {
        Serial.print(profil.zracnaVlagaMin, 1);
        Serial.print("-");
        Serial.print(profil.zracnaVlagaMax, 1);
        Serial.println(" %");
    }
    else
    {
        Serial.println("se ne ocenjuje");
    }

    Serial.print("Svetloba: ");
    if (profil.svetlobaMin == profil.svetlobaMax)
    {
        Serial.println(besediloSvetlobe(profil.svetlobaMin));
    }
    else
    {
        Serial.print(besediloSvetlobe(profil.svetlobaMin));
        Serial.print("-");
        Serial.println(besediloSvetlobe(profil.svetlobaMax));
    }

    Serial.print("Priporoceno trajanje: ");
    if (profil.preverjajUreSvetlobe)
    {
        Serial.print(profil.svetlobaUreMin, 1);
        Serial.print("-");
        Serial.print(profil.svetlobaUreMax, 1);
        Serial.println(" h");
    }
    else
    {
        Serial.println("se ne ocenjuje");
    }

    Serial.print("Zemlja: ");
    Serial.println(besediloZemlje(profil.nacinZemlje));

    Serial.print("Vir: ");
    Serial.println(profil.vir);
}

// Izbere rastlino po indeksu, ponastavi stevce in takoj osvezi zaslon
void izberiRastlino(int indeks)
{
    izbranaRastlina = indeks;
    ponastaviOpozorilaInDan();
    izpisiProfilRastline(RASTLINE[izbranaRastlina]);
    osveziOled(trenutnaMeritev);
}

void izberiNaslednjoRastlino()
{
    int naslednji = izbranaRastlina + 1;

    if (naslednji >= STEVILO_RASTLIN)
    {
        naslednji = 0;
    }

    izberiRastlino(naslednji);
}


// =====================================================================
//  12. TESTNI NACIN IN KONFERENCNI PRIKAZ
// =====================================================================
// Testni nacin sluzi samo predstavitvi opozoril. Simulirane vrednosti se
// nikoli ne zapisejo v CSV in ne vplivajo na stevce normalnega nacina.

const char *besediloTestneSimulacije(TestnaSimulacija simulacija)
{
    if (simulacija == TEST_NIZKA_TEMPERATURA)
    {
        return "TEMPERATURA";
    }
    if (simulacija == TEST_NIZKA_VLAGA_ZRAKA)
    {
        return "ZRACNA VLAGA";
    }
    if (simulacija == TEST_SUHA_ZEMLJA)
    {
        return "VLAGA ZEMLJE";
    }
    if (simulacija == TEST_VISOKA_SVETLOBA)
    {
        return "NEPOSREDNA SVETLOBA";
    }
    if (simulacija == TEST_NIZKA_DNEVNA_SVETLOBA)
    {
        return "DNEVNA SVETLOBA NIZKA";
    }
    return "";
}

// Sporocilo avtomatskega prikaza je odvisno samo od preteklega casa.
// Sedem korakov po 10 s, nato prikaz obstoji na "TEST KONCAN".
const char *besediloAvtoDemo(unsigned long preteceniMs)
{
    if (preteceniMs < DEMO_KORAK_MS * 1) return "TESTNI NACIN";
    if (preteceniMs < DEMO_KORAK_MS * 2) return "TEMP PRENIZKA";
    if (preteceniMs < DEMO_KORAK_MS * 3) return "VLAGA PRENIZKA";
    if (preteceniMs < DEMO_KORAK_MS * 4) return "ZEMLJA SUHA";
    if (preteceniMs < DEMO_KORAK_MS * 5) return "PREMOCNA SVETLOBA";
    if (preteceniMs < DEMO_KORAK_MS * 6) return "PREMALO SVETLOBE";
    if (preteceniMs < DEMO_KORAK_MS * 7) return "POGOJI PONOVNO USTREZNI";
    return "TEST KONCAN";
}

// Naslov TESTNI NACIN je ves cas na vrhu, da je jasno, da ne gre za
// prave meritve.
void izpisiOledTestniNacin()
{
    zaslon.clearDisplay();
    zaslon.setTextColor(SSD1306_WHITE);
    zaslon.setTextSize(1);

    zaslon.setCursor(0, 0);
    zaslon.print("TESTNI NACIN");
    zaslon.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    zaslon.setCursor(0, 13);
    zaslon.print("Rastlina: ");
    zaslon.print(RASTLINE[izbranaRastlina].kratkoIme);

    zaslon.setCursor(0, 30);

    if (avtoDemoAktiven)
    {
        zaslon.println(besediloAvtoDemo(millis() - avtoDemoZacetek));
    }
    else if (testnaSimulacija == TEST_NIC)
    {
        zaslon.println("Ni simulacije");
        zaslon.setCursor(0, 45);
        zaslon.println("(ukazi TT..TX ali DEMO)");
    }
    else if (!testniAlarmAktiven)
    {
        zaslon.println("Simulacija tece...");
        zaslon.setCursor(0, 45);
        zaslon.print("Stevec tikov: ");
        zaslon.print(testniStevec);
    }
    else
    {
        zaslon.println("OPOZORILO (TEST):");
        zaslon.setCursor(0, 45);
        zaslon.println(besediloTestneSimulacije(testnaSimulacija));
    }

    zaslon.display();
}

void zazeniAvtoDemo()
{
    avtoDemoAktiven = true;
    avtoDemoZacetek = millis();

    testnaSimulacija = TEST_NIC;
    testniStevec = 0;
    testniAlarmAktiven = false;

    Serial.println("[DEMO] Avtomatski konferencni prikaz zagnan (7 korakov po 10 s).");
}

void izberiTestnoSimulacijo(TestnaSimulacija nova)
{
    avtoDemoAktiven = false;   // rocni ukaz prevzame nadzor nad prikazom

    testnaSimulacija = nova;
    testniStevec = 0;
    testniAlarmAktiven = (nova == TEST_NIZKA_DNEVNA_SVETLOBA);

    Serial.print("[TEST] Simulacija: ");
    Serial.println(nova == TEST_NIC ? "brez (normalno stanje)"
                                    : besediloTestneSimulacije(nova));
}

// Klice se na INTERVAL_MERITVE_MS in stopnjuje rocno izbrano simulacijo
void posodobiTestniNacin()
{
    if (testnaSimulacija == TEST_NIC || testnaSimulacija == TEST_NIZKA_DNEVNA_SVETLOBA)
    {
        return;   // ni kaj stopnjevati oziroma je ze aktivno
    }

    testniStevec++;

    int potrebnoTikov = 0;

    if (testnaSimulacija == TEST_NIZKA_TEMPERATURA)
    {
        potrebnoTikov = TEST_OKNO_TEMPERATURA;
    }
    else if (testnaSimulacija == TEST_VISOKA_SVETLOBA)
    {
        potrebnoTikov = TEST_OKNO_SVETLOBA;
    }
    else if (testnaSimulacija == TEST_NIZKA_VLAGA_ZRAKA)
    {
        potrebnoTikov = TEST_OKNO_VLAGA_ZRAKA;
    }
    else if (testnaSimulacija == TEST_SUHA_ZEMLJA)
    {
        potrebnoTikov = TEST_OKNO_VLAGA_ZEMLJE;
    }

    if (testniStevec >= potrebnoTikov)
    {
        testniAlarmAktiven = true;
    }
}

void vklopiTestniNacin()
{
    aktivenTestniNacin = true;

    Serial.println("[TEST] vklopljen");

    zazeniAvtoDemo();

    Serial.println("Avtomatski prikaz tece sam. Na voljo so tudi rocni ukazi:");
    Serial.println("  TT - prenizka temperatura");
    Serial.println("  TV - prenizka zracna vlaga");
    Serial.println("  TZ - presuh substrat (zemlja)");
    Serial.println("  TL - previsoka / neposredna svetloba");
    Serial.println("  TD - prenizka dnevna svetloba");
    Serial.println("  TN - vrnitev v normalno stanje (testni nacin ostane vklopljen)");
    Serial.println("  DEMO - znova zazene avtomatski konferencni prikaz");
    Serial.println("  TX - izklop testnega nacina (nazaj na prave senzorje)");
}

void izklopiTestniNacin()
{
    aktivenTestniNacin = false;
    avtoDemoAktiven = false;
    testnaSimulacija = TEST_NIC;
    testniStevec = 0;
    testniAlarmAktiven = false;

    Serial.println("[TEST] izklopljen");
}

void preklopiTestniNacin()
{
    if (aktivenTestniNacin)
    {
        izklopiTestniNacin();
    }
    else
    {
        vklopiTestniNacin();
    }
}


// =====================================================================
//  13. TIPKA
// =====================================================================
// Kratek pritisk izbere naslednjo rastlino, dolg pritisk (>= 2 s) pa
// preklopi testni nacin. Dolg pritisk zaznamo ze med drzanjem, da ima
// uporabnik takojsnjo povratno informacijo.

void preveriTipko()
{
    bool surovoStanje = digitalRead(PIN_TIPKA);

    // Ob vsaki spremembi surovega stanja zacnemo znova meriti, kako dolgo
    // je stanje ze stabilno (odpravljanje odskoka kontaktov)
    if (surovoStanje != tipkaSurovoPrejsnje)
    {
        tipkaCasSpremembe = millis();
        tipkaSurovoPrejsnje = surovoStanje;
    }

    if (millis() - tipkaCasSpremembe > TIPKA_DEBOUNCE_MS && surovoStanje != tipkaStabilno)
    {
        tipkaStabilno = surovoStanje;

        if (tipkaStabilno == LOW)
        {
            // Pri INPUT_PULLUP pomeni LOW pritisnjeno tipko
            tipkaCasPritiska = millis();
            tipkaDrzimo = true;
            tipkaDolgSprozen = false;
            Serial.println("[TIPKA] pritisnjena");
        }
        else if (tipkaDrzimo)
        {
            // Spust brez ze sprozenega dolgega pritiska = kratek pritisk
            if (!tipkaDolgSprozen)
            {
                Serial.println("[TIPKA] kratki pritisk");
                izberiNaslednjoRastlino();
            }

            tipkaDrzimo = false;
        }
    }

    if (tipkaDrzimo && !tipkaDolgSprozen && millis() - tipkaCasPritiska >= TIPKA_DOLG_PRITISK_MS)
    {
        tipkaDolgSprozen = true;
        Serial.println("[TIPKA] dolgi pritisk");
        preklopiTestniNacin();
    }
}


// =====================================================================
//  14. SERIJSKI UKAZI
// =====================================================================
//   L      seznam vseh CSV datotek
//   P      izpis trenutne (aktivne) datoteke
//   P001   izpis datoteke /meritve_001.csv (enako P002 ... P999)
//   R      seznam rastlin
//   R1     izbira rastline (enako R2, R3)
//   BRISI  izbrise vse seje in ponovno zazene ESP32
//   DEMO   zazene avtomatski konferencni prikaz
//   TT TV TZ TL TD TN TX   rocne simulacije v testnem nacinu

void preveriSerijskiUkaz()
{
    if (!Serial.available())
    {
        return;
    }

    String ukaz = Serial.readStringUntil('\n');
    ukaz.trim();

    if (ukaz.length() == 0)
    {
        return;
    }

    if (ukaz == "L")
    {
        izpisiSeznamDatotek();
    }
    else if (ukaz == "P")
    {
        izpisiDatoteko(csvPot);
    }
    else if (ukaz.length() == 4 && ukaz.charAt(0) == 'P' &&
             isDigit(ukaz.charAt(1)) && isDigit(ukaz.charAt(2)) && isDigit(ukaz.charAt(3)))
    {
        char pot[32];
        snprintf(pot, sizeof(pot), "/meritve_%c%c%c.csv",
                 ukaz.charAt(1), ukaz.charAt(2), ukaz.charAt(3));
        izpisiDatoteko(pot);
    }
    else if (ukaz == "R")
    {
        izpisiSeznamRastlin();
    }
    else if (ukaz.length() == 2 && ukaz.charAt(0) == 'R' && isDigit(ukaz.charAt(1)))
    {
        int stevilka = ukaz.charAt(1) - '0';

        if (stevilka >= 1 && stevilka <= STEVILO_RASTLIN)
        {
            izberiRastlino(stevilka - 1);   // v polju stejemo od 0
        }
        else
        {
            Serial.println("NAPAKA: rastlina ne obstaja. Uporabi R1, R2 ali R3.");
        }
    }
    else if (ukaz == "BRISI")
    {
        Serial.println("Brisem vse CSV seje (/meritve_001.csv .. /meritve_999.csv) ...");
        Serial.print("Izbrisanih datotek: ");
        Serial.println(izbrisiVseSeje());
        Serial.println("Cez 1 sekundo sledi ponovni zagon (nova cista seja /meritve_001.csv) ...");

        delay(1000);
        ESP.restart();
    }
    else if (ukaz == "DEMO")
    {
        // Rezerva, ce tipka na predstavitvi ne bi delovala
        if (!aktivenTestniNacin)
        {
            vklopiTestniNacin();
        }
        else
        {
            zazeniAvtoDemo();
        }
    }
    else if (ukaz == "TX")
    {
        izklopiTestniNacin();
    }
    else if (ukaz == "TT" || ukaz == "TV" || ukaz == "TZ" ||
             ukaz == "TL" || ukaz == "TD" || ukaz == "TN")
    {
        if (!aktivenTestniNacin)
        {
            Serial.println("NAPAKA: testni nacin ni vklopljen. Za vklop pridrzi tipko vsaj 2 sekundi.");
        }
        else if (ukaz == "TT") izberiTestnoSimulacijo(TEST_NIZKA_TEMPERATURA);
        else if (ukaz == "TV") izberiTestnoSimulacijo(TEST_NIZKA_VLAGA_ZRAKA);
        else if (ukaz == "TZ") izberiTestnoSimulacijo(TEST_SUHA_ZEMLJA);
        else if (ukaz == "TL") izberiTestnoSimulacijo(TEST_VISOKA_SVETLOBA);
        else if (ukaz == "TD") izberiTestnoSimulacijo(TEST_NIZKA_DNEVNA_SVETLOBA);
        else                   izberiTestnoSimulacijo(TEST_NIC);
    }
    else
    {
        Serial.println("Neznan ukaz. Na voljo: L, P, P001..P999, R, R1..R3, BRISI, TT,TV,TZ,TL,TD,TN,TX, DEMO");
    }
}


// =====================================================================
//  15. ZAGON
// =====================================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("==============================");
    Serial.println("ZAGON SISTEMA ZA RASTLINE");
    Serial.println("==============================");

    Wire.begin(PIN_SDA, PIN_SCL);

    // OLED
    oledDeluje = zaslon.begin(SSD1306_SWITCHCAPVCC, OLED_NASLOV);

    if (oledDeluje)
    {
        Serial.println("OLED 0x3C: DELUJE");

        // Zaslon je vgrajen obrnjen, zato prikaz zavrtimo za 180 stopinj
        zaslon.setRotation(2);

        zaslon.clearDisplay();
        zaslon.setTextColor(SSD1306_WHITE);
        zaslon.setTextSize(1);
        zaslon.setCursor(0, 10);
        zaslon.println("PLANT MONITOR");
        zaslon.println();
        zaslon.println("ZAGON SENZORJEV...");
        zaslon.display();
    }
    else
    {
        Serial.println("OLED: NAPAKA");
    }

    // BH1750
    bh1750Deluje = senzorSvetlobe.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    Serial.println(bh1750Deluje ? "BH1750 0x23: DELUJE" : "BH1750: NAPAKA");

    // DHT11
    senzorDHT.begin();
    Serial.println("DHT11: ZAGNAN");

    // Kapacitivni senzor zemlje
    pinMode(PIN_ZEMLJA, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_ZEMLJA, ADC_11db);
    Serial.println("SENZOR ZEMLJE GPIO34: ZAGNAN");

    // Tipka (zunanji upor ni potreben)
    pinMode(PIN_TIPKA, INPUT_PULLUP);
    tipkaSurovoPrejsnje = digitalRead(PIN_TIPKA);
    tipkaStabilno = tipkaSurovoPrejsnje;
    Serial.println("TIPKA GPIO27: PRIPRAVLJENA");

    // LittleFS: vsak zagon dobi svojo datoteko seje.
    // Argument true pomeni, da se ob neuspesnem priklopu sistem formatira.
    if (LittleFS.begin(true))
    {
        Serial.println("LittleFS: DELUJE");

        if (poisciProstoImeDatoteke(csvPot, sizeof(csvPot)))
        {
            littlefsDeluje = pripraviCsvDatoteko();

            if (littlefsDeluje)
            {
                Serial.print("[CSV] Nova seja: ");
                Serial.println(csvPot);
            }
        }
        else
        {
            littlefsDeluje = false;
            Serial.println("NAPAKA: vsa imena /meritve_001.csv .. /meritve_999.csv so zasedena");
        }
    }
    else
    {
        littlefsDeluje = false;
        Serial.println("NAPAKA: LittleFS se ni zagnal - CSV zapisovanje izklopljeno, merjenje deluje naprej");
    }

    Serial.print("[RASTLINA] Privzeto izbrana: ");
    Serial.println(RASTLINE[izbranaRastlina].ime);

    if (aktivenTestniNacin)
    {
        vklopiTestniNacin();
    }

    Serial.println("==============================");
}


// =====================================================================
//  16. GLAVNA ZANKA
// =====================================================================
// Zanka ne uporablja blokirnih delay() klicev. Namesto tega ob vsakem
// obhodu preveri, ali je od zadnjega opravila minilo dovolj casa.

void loop()
{
    static unsigned long casZadnjeMeritve = 0;
    static unsigned long casZadnjegaZapisa = 0;

    unsigned long trenutniCas = millis();

    preveriSerijskiUkaz();
    preveriTipko();   // ob vsakem obhodu, sicer odpravljanje odskoka ni zanesljivo

    // 1) Osvezitev senzorjev in zaslona
    if (trenutniCas - casZadnjeMeritve >= INTERVAL_MERITVE_MS)
    {
        casZadnjeMeritve = trenutniCas;

        // Prave meritve tecejo tudi v testnem nacinu
        trenutnaMeritev = preberiSenzorje();

        if (aktivenTestniNacin && !avtoDemoAktiven)
        {
            posodobiTestniNacin();
        }

        izpisiSerial(trenutnaMeritev);
        osveziOled(trenutnaMeritev);
    }

    // 2) Shranjevanje zapisa in presoja opozoril.
    //    Tece vedno iz pravih meritev; testni nacin na to ne vpliva.
    if (trenutniCas - casZadnjegaZapisa >= INTERVAL_ZAPISA_MS)
    {
        casZadnjegaZapisa = trenutniCas;

        if (steviloZapisov < MAX_ZAPISOV)
        {
            steviloZapisov++;
        }
        steviloVsehZapisov++;

        preveriOpozorila(trenutnaMeritev);
        posodobiDnevnoSvetlobo(trenutnaMeritev);

        char csvVrstica[160];
        sestaviCsvVrstico(csvVrstica, sizeof(csvVrstica), trenutnaMeritev, steviloVsehZapisov);

        bool csvOk = zapisiVrsticoVDatoteko(csvVrstica);

        Serial.printf("[SHRANJENO] zapis %lu | N=%d/%d | CSV: %s\n",
                      steviloVsehZapisov, steviloZapisov, MAX_ZAPISOV,
                      csvOk ? "DA" : "NAPAKA");
        Serial.println(csvVrstica);
    }
}
