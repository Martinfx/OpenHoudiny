# Látky, lana a měkká tělesa: Cloth Solver (XPBD)

**Cloth Solver** simuluje všechno, co drží pohromadě jen vazbami mezi body:
ubrus, prapor, oponu, lano, gumový míč nebo polštář. Počítá metodou XPBD
(*Extended Position Based Dynamics*), stejně jako Vellum v Houdini. Body
geometrie jsou hmotné body. Hrany, úhlopříčky a ohyby jsou vazby, které
drží délku, tvar a přehyby. Uzavřená síť může navíc držet objem.

![Ubrus spadne na stůl s mísou a na něj měkký míč](img/tablecloth.jpg)

```
./build/prototype --example tablecloth                 # v editoru: Play
./build/prototype sim tablecloth ubrus.mp4 --frames 120
./build/prototype sim flag vlajka.png --every 15 --frames 90
```

## 1. Příklady

### Ubrus a míč

Příklad **tablecloth** ([examples/sim/tablecloth.pgsim](../examples/sim/tablecloth.pgsim)):

- **Grid** 1,8 × 1,5 m (38 × 45 bodů) posunutý Transformem do výšky 1,15 m.
  Primitive Wrangle `checks` obarví jeho plochy do červenobílých kostek
  (`@P` je v Primitive Wrangle střed plochy).
- **Sphere** o poloměru 17 cm, výš a trochu stranou. Primitive Wrangle
  `blue` ji obarví.
- **Merge** spojí obojí do jedné geometrie pro **Cloth Solver**. Grid je
  otevřený, takže je z něj látka. Koule je uzavřená a Pressure 1 z ní
  udělá míč, který drží svůj objem.
- **Colliders**: deska stolu, čtyři nohy a mísa na stole (objekty Object).

Ubrus padá, vzduch ho brzdí a okraje se vlní. Přehne se přes hrany stolu
a přes mísu. Rohy visí a skládají se do záhybů. Míč dopadne na ubrus,
promáčkne ho, sklouzne a skutálí se na zem. Simulace trvá asi 36 ms na
snímek (2 000 bodů, 20 podkroků).

### Vlajka ve větru

Příklad **flag** ([examples/sim/flag.pgsim](../examples/sim/flag.pgsim)):

- Grid 1,8 × 1,2 m postavený na výšku.
- Point Wrangle nastaví body u žerdi na `i@pin = @P.x < 0.07;`, takže
  jsou přišpendlené.
- **Wind** s nárazy (gusts 0,7) fouká 8 m/s.

Vlajka se rozvine, třepotá se a nárazy větru jí probíhají jako vlny.
Simulace trvá asi 15 ms na snímek.

![Vlajka ve větru](img/flag.jpg)

## 2. Co je čím

Geometrie zapojená do vstupu **Geometry** určuje, co se simuluje:

| Geometrie | Co z ní je | Vazby |
|---|---|---|
| polygony (grid, cokoli z ploch) | látka | hrany drží délku (Stretch), úhlopříčky čtyřúhelníků tvar (smyk), body přes společnou hranu dvou trojúhelníků vzdálenost (Bend) |
| otevřené polyčáry | lano | úseky drží délku, bod a ob-jeden drží vzdálenost (ohyb) |
| uzavřená síť (sphere, box) s Pressure > 0 | balon, polštář, měkké těleso | jako látka a navíc objem = Pressure × objem v klidu |

Body s atributem `pin` = 1 se samy nehýbou. Jdou tam, kde je má
geometrie v aktuálním snímku, takže animovaná geometrie (třeba
klíčovaný Transform) je nese s sebou: vlajku na stožáru nebo záclonu na
posuvné tyči. Mezi snímky jde přišpendlený bod plynule, po podkrocích.

Hmotnost bodu je hustota (Density, kg/m², u lana kg/m) krát plocha
kolem něj. Hmotnost lze zadat i přímo bodovým atributem `mass` (kg).
Počáteční rychlost bere řešič z atributu `v`, pokud ho geometrie má.

## 3. Parametry

| Parametr | Výchozí | Význam |
|---|---|---|
| Density | 0,3 kg/m² | 0,1 hedvábí, 0,3 bavlna, 0,8 plátno |
| Stretch | 10 000 N/m | jak drží hrana délku: 10⁴ bavlna, stovky guma |
| Bend | 1 N/m | tuhost v ohybu: 0,1 hedvábí, 1 bavlna, 10 plátno, 1000 karton |
| Pressure | 0 | uzavřené sítě drží tento podíl klidového objemu; 0 = látka |
| Thickness | 0,01 m | jak daleko od podlahy, objektů a sebe sama látka zůstane |
| Friction | 0,4 | 0 klouže, 1 drží |
| Self Collision | zapnuto | látka neprojde sama sebou |
| Floor | zapnuto | podlaha ve výšce 0 |
| Air Drag | 1 | jak silně tlačí vzduch: stojící vzduch při pádu, vítr, proud plynu |
| Damping | 0,5 1/s | jak rychle pohyb sám od sebe utichá |
| Gravity | 9,81 m/s² | |
| Substeps | 20 | podkroků na snímek; víc = tužší a stabilnější |
| Color | cihlová | barva ploch bez vlastního `Cd` |

Tuhosti jsou fyzikální (N/m) a nezávisí na počtu podkroků ani na hustotě
sítě. Víc podkroků látku jen přiblíží tomu, co tuhosti říkají.

**Colliders** berou objekty (Object, i animované) a **kusy RBD Solveru**:
trosky padající na plachtu nebo plachta přehozená přes padající kusy.
**Forces** berou vítr (Wind). Když je ve scéně Pyro Solver, látku unáší
i proud jeho plynu, třeba horký vzduch nad ohněm.

Výstup **Look** se zapojí do Output, takže se látka simuluje a kreslí.
Výstup **Cloth** vede do uzlu **Cloth Geometry**, který vrátí látku zpět
jako geometrii (body s `v` a normálami `N`) pro další uzly, export nebo
USD.

## 4. Jak to funguje

Krok podle Macklin a kol., *Small Steps in Physics Simulation* (2019):
snímek se dělí na podkroky (Substeps) a v každém je právě jeden průchod
přes vazby. Tento postup konverguje lépe než mnoho iterací v jednom
velkém kroku.

1. **Předpověď:** rychlost + gravitace + vzduch → nová poloha, tlumení.
2. **Vazby délky** (stretch, smyk, ohyb) s poddajností α = 1/tuhost,
   v podkroku α̃ = α/h². Smyk je 10⁴× poddajnější než délka hrany, aby
   úhlopříčky čtyřúhelníků nedělaly z látky prkno.
3. **Objem balonů:** gradient objemu podle bodu je šestina součtu
   vektorových součinů zbylých dvou vrcholů každého jeho trojúhelníku.
4. **Samokolize:** body v hašované mřížce se odtlačí na dvojnásobek
   poloměru (Thickness, nebo 0,3 průměrné hrany, je-li to víc). Sousedé
   spojení vazbou se nekontrolují. Body, které byly blíž už v klidu
   (kolem pólu koule), se drží jen na své klidové vzdálenosti. Jinak by
   se pól koule promáčkl.
5. **Kolize** s podlahou a objekty (vzdálenostní funkce tvarů) a tření:
   posun podél povrchu se ubere úměrně hloubce průniku.
6. **Rychlost** = (nová poloha − stará) / h.

**Vzduch** tlačí na každý trojúhelník jako na destičku podél jeho normály:
½ ρ C_d A |v·n| (v·n), kde v je rychlost vzduchu vůči ploše. Síla je
omezená tak, aby v jednom podkroku nepřetočila pohyb proti vzduchu.
Stojící vzduch brzdí padající látku: plachta padá plochou nanejvýš
2 m/s. Uzavřená síť dostává vzduch jen zvenku, tedy jen na plochy, do
kterých vzduch skutečně naráží. Míč tak padá jako míč, ne jako chomáč
destiček.

**Determinismus** (invariant I5): vazby jsou hladově obarvené do nejvýše
64 barev tak, aby žádné dvě vazby jedné barvy nesdílely bod. Každá barva
se řeší paralelně, zbytek a balony sériově v pevném pořadí. Výsledek je
bit po bitu stejný na jednom i na čtyřech vláknech.

## 5. Snímky, cache a checkpoint

- Snímek (`Frame::cloth`) nese polohy bodů a rychlosti v půlkách floatu.
  Cache snímků je od verze 11 i s látkou. Geometrie v klidu se na disk
  neukládá, protože je v síti. Při čtení z cache ji snímkům vrátí
  `adoptCloth` (CLI `--from-cache`, editor, Python).
- Checkpoint bake obsahuje polohy a rychlosti přesně (float), takže
  pokračování je bit po bitu stejné jako simulace bez přerušení.
- Profil kroku (Frame::Profile) má vlastní položku **Cloth**.

## 6. Ověřování

`tests/test_cloth.cpp`:

- lano se zhoupne dolů a drží délku;
- látka se přehne přes kouli a zůstane na ní ležet;
- přišpendlené rohy stojí a zbytek visí;
- balon drží objem, bez Pressure je z něj látka;
- vítr zvedne vlajku;
- 1 a 4 vlákna a obnova ze stavu dají totéž bit po bitu;
- měkký míč zůstane kulatý i tam, kde se body na pólech tlačí k sobě;
- plachta padá stojícím vzduchem pomaleji než volným pádem;
- příklad tablecloth: síť → simulace → snímek zapsaný a přečtený
  (formát v11, `adoptCloth`) → checkpoint, který pokračuje bit po bitu.

Příklady projdou testem formátu a testem „všechny příklady běží“.

## 7. Omezení

- Kolize se kontrolují jen mezi body. Hrana může projít hranou, když je
  síť hrubá vůči tloušťce. Vellum kontroluje i hrany a trojúhelníky.
- Látka netlačí zpět na kusy RBD. Kusy na ni působí, ona na ně ne.
- Látka se netrhá (Vellum má *tearing*).
- Žádné granuláty ani tvarové vazby (*shape matching*). Měkká tělesa jsou
  zatím jen balony.
- Renderer kreslí látku neprůsvitnou, bez prosvítání tenké tkaniny.

## 8. Odkazy

- M. Macklin, M. Müller, N. Chentanez: *XPBD: Position-Based Simulation of
  Compliant Constrained Dynamics*, MIG 2016.
- M. Macklin, K. Storey, M. Lu a kol.: *Small Steps in Physics Simulation*,
  SCA 2019.
- M. Müller a kol.: *Position Based Dynamics*, VRIPHYS 2006.
