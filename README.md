# Procedurální geometrické jádro

Prototyp jádra open-source procedurálního systému pro zpracování geometrie:
node-based, nedestruktivní, headless-first.

- **[ROADMAP.md](ROADMAP.md)** — fáze, milníky, rozhodovací brány, rizika
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — datový model, cook engine, invarianty

> **Jméno je zatím placeholder.** Název `OpenHoudiny` je zaměnitelně podobný
> registrované ochranné známce SideFX; jmenný prostor v kódu je proto neutrální
> `pg`. Volba jména je blokující úkol fáze 0 roadmapy.

## Co to je

Prototyp **není produkt**. Existuje proto, aby měřením ověřil čtyři tvrzení,
na kterých architektura stojí, dokud je ještě levné je vyvrátit:

1. **Copy-on-write na atributových polích** udrží paměť v mezích u dlouhých
   řetězců uzlů.
2. **Líná pull evaluace s verzováním** dělá interaktivní editaci možnou.
3. **Deterministické chunkování** dá bitově stejný výsledek nezávisle na
   počtu vláken.
4. **Per-element jazyk** vázaný na sloty je použitelný a měřitelný baseline
   pro budoucí JIT.

Jedno z kritérií roadmapy prototyp rovnou vyvrátil — viz poznámku u M5.

## Build

Bez externích závislostí: stačí C++20 a standardní knihovna.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

Se sanitizery:

```bash
cmake -S . -B build-asan -DPG_SANITIZE=ON       && cmake --build build-asan && ./build-asan/pgtests
cmake -S . -B build-tsan -DPG_SANITIZE_THREAD=ON && cmake --build build-tsan && ./build-tsan/pgtests
```

## Spuštění

```bash
./build/pgtests            # 50 testů proti invariantům
./build/pgbench            # měření tvrzení výše
./build/pgdemo out.obj --frames 24
```

`pgdemo` postaví graf `grid → pointwrangle → groupbox → blast → transform`,
cookne ho, vypíše tabulku atributů ve stylu geometry spreadsheetu a zapíše OBJ,
který jde otevřít v Blenderu nebo kdekoliv jinde.

## Příklad snippetu jazyka

```c
@P.y = noise(@P * 0.45 + vec3(@Time, 0.0, 0.0)) * 2.0 - 1.0;
@height = @P.y;
@Cd = vec3(fit(@P.y, -1.0, 1.0, 0.1, 1.0), 0.4, 0.8);
```

Typ vytvářeného atributu se odvodí z pravé strany: `@height` vznikne jako
`float`, `@Cd` jako `vec3`. Uzel se sám označí za časově závislý, protože
snippet čte `@Time`.

## Stav

Hotovo a otestováno: COW geometrie, cook engine, časová závislost, LRU cache,
deterministický paralelismus, per-element jazyk, 10 typů uzlů, 50 testů
(čisté pod ASan, UBSan i ThreadSanitizerem).

Vědomě chybí: I/O (USD, Alembic, VDB), JIT, packed primitives, digital assets,
serializace scény, Python vazby, GUI, simulace. Podrobně v
[ARCHITECTURE.md §9](ARCHITECTURE.md#9-co-prototyp-skutečně-umí).

## Licence

Apache 2.0 (viz [ROADMAP.md §0.1](ROADMAP.md#01-právní-a-identita)).
Implementace je clean room — žádný HDK, žádný reverse engineering.
