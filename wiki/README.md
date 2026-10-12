# Wiki du générateur d'île

Ce wiki explique comment le projet fonctionne : la structure du code, le parcours des
données, et le détail de chaque étape de génération. Le [README racine](../README.md) reste la
notice d'utilisation (build, commandes, contrôles du viewer).

| Page | Contenu |
|------|---------|
| [Vue d'ensemble](README.md) (cette page) | Architecture, flux de données, concepts transverses |
| [Étape 1 : paramètres](etape-1-parametres.md) | Lecture de la config, tirage depuis la seed, overrides, validation |
| [Étape 2 : initialisation de l'île](etape-2-initialisation.md) | Masque, bathymétrie, topographie, montagnes, géologie |
| [Étape 3 : érosion](etape-3-erosion.md) | Érosion hydraulique par particules, repère normalisé, lecture pas à pas |
| [Étapes 4 et 5 : upscale et hydrologie](etape-5-hydrologie.md) | Terrain aux blocs, rivières (sources, confluences) et lacs |
| [Étape 7 : voxelisation et export Minecraft](etape-7-voxelisation.md) | Hauteurs → blocs, monde Minecraft Java (Anvil) |

---

## 1. L'idée générale

On génère un monde **fini** (une île sur une carte de `world.width × world.length` blocs),
d'un seul tenant plutôt que chunk par chunk. Ça permet des traitements globaux impossibles en
génération infinie, comme l'érosion. Le monde est d'abord simulé en 2D (altitudes, masques,
champs physiques) sur une grille de simulation, puis il sera transformé en blocs 3D. Rien n'est propre à Minecraft : les exporteurs traduiront le résultat vers chaque
format.

La génération est une suite d'**étapes** (le *pipeline*). Chacune lit l'état produit par les
précédentes et y ajoute ou modifie des données :

```
config (.jsonc)
   │  WorldConfig::load            ← résolution de TOUS les paramètres (étape 1, voir sa page)
   ▼
WorldConfig { seed, raw, params }
   │  Pipeline::fromConfig         ← liste des étapes (défaut : StageRegistry::defaultPipeline)
   ▼
Pipeline ── run() ──► étape 1 ──► étape 2 ──► … ──► étape N
                         │           │                │
                         ▼           ▼                ▼
                     Snapshot    Snapshot  …      Snapshot      (un état complet par étape)
                         │
              ┌──────────┴───────────┐
              ▼                      ▼
        wgen (CLI)              wview (viewer)
   .wgw, PNG, params.json   affichage live, étape par étape
```

## 2. Organisation du code

```
src/wg/                       bibliothèque "wg", sans dépendance au rendu
  core/
    Config         WorldConfig : charge le .jsonc, lit la seed, lance la résolution des paramètres
    WorldInfo      dimensions du monde (blocs), niveau de la mer, seed, taille d'un bloc
    WorldState     l'état du monde : paramètres + calques 2D + voxels (copy-on-write)
    LayerMeta      style d'affichage d'un calque (hauteur, scalaire, divergent, catégoriel)
    Grid2D         grille 2D dense générique
    VoxelVolume    blocs 3D en sections 16³ (copy-on-write, sections uniformes compactes)
    Block          palette de blocs générique
    Random         RNG portable (splitmix64), hash de chaînes, dérivation de sous-seeds
  params/
    IslandParams   struct typée de tous les paramètres + leurs distributions (étape 1)
    ParamResolver  mécanique de tirage par paramètre, overrides, validation
  gen/
    Stage          interface d'une étape + StageContext (seed, progression)
    StageParams    paramètres propres à une entrée du "pipeline" JSON (rarement utilisé)
    StageRegistry  "type" → fabrique d'étape, et pipeline par défaut
    Pipeline       construit et exécute les étapes, produit les snapshots, gère live/annulation
    stages/        les étapes : ParametersStage, IslandInitStage, ErosionStage, UpscaleStage,
                   HydrologyStage, PostprocessStage, VoxelizeStage, BuiltinStages.cpp (registre)
  island/
    IslandLayers   noms des calques de l'île et flags de cellule (contrat entre étapes)
    IslandGrid     géométrie de la grille de simulation (taille, cellule, coordonnées)
    Geology        strates et plis → material(x, y, z)
    Relief         zones de montagnes × bruit ridged
  noise/Fbm        bruit fractal OpenSimplex2 (FastNoiseLite)
  noise/Ridged     bruit ridged multifractal (Musgrave)
  erosion/         érosion par particules : ErosionMap (cellules), Drop (une goutte), ParticleErosion (les pas)
  render/          vues, palettes, rendu 2D (partagé par le PNG et le viewer)
  io/              format natif .wgw, export PNG, NBT et export Minecraft (Anvil)
apps/
  wgen/            CLI : config → .wgw / PNG / params.json / monde Minecraft
  wmc/             CLI : .wgw → monde Minecraft Java
  wview/           viewer raylib : GenerationJob (thread), Meshes (3D), main (UI)
tests/main.cpp     tests unitaires (sans framework)
```

## 3. Concepts transverses

### 3.1 `WorldState` : l'état du monde

[WorldState.hpp](../src/wg/core/WorldState.hpp) contient tout ce que le pipeline produit :

| Contenu | Accès | Produit par |
|---------|-------|-------------|
| Paramètres résolus (`ResolvedParams`) | `world.params().values` | étape `parameters` |
| Calques `float` nommés (`h`, `mask`…) + leur `LayerMeta` | `layer(name)`, `editLayer(name)`, `createLayer(...)` | étapes 2D |
| Calques d'octets (`flags`) | `byteLayer`, `editByteLayer`, `createByteLayer` | étapes 2D |
| Volume de voxels | `voxels()`, `editVoxels()`, `createVoxels()` | voxelisation (à venir) |

Un calque a sa propre taille (la grille de simulation 1024², par exemple) et couvre toujours toute
la carte. Le viewer l'étire sur le monde, et `cellSizeM()` donne la taille d'une cellule en
mètres.

**Copy-on-write.** Les données sont tenues par des `shared_ptr`, donc copier un `WorldState`
ne copie que des pointeurs. `edit*()` duplique la donnée si quelqu'un d'autre la partage. C'est
ce qui rend les snapshots quasi gratuits : un calque que l'étape ne touche pas reste partagé
entre tous les snapshots. Côté voxels, la même logique s'applique section par section.

### 3.2 Pipeline, étapes et snapshots

Une étape ([Stage.hpp](../src/wg/gen/Stage.hpp)) implémente
`run(WorldState&, const StageContext&)`. Le contexte fournit :

- `config` : la config chargée, dont les paramètres résolus ;
- `info` : les dimensions du monde ;
- `seed` : une graine propre à l'étape, `deriveSeed(seed, nom)` ;
- `progress(f)` : rapporte l'avancement (voir 3.4).

`Pipeline::run` ([Pipeline.cpp](../src/wg/gen/Pipeline.cpp)), pour chaque étape :

1. garde une copie `before` de l'état, ce qui force le copy-on-write ;
2. exécute l'étape ;
3. compare les identités des données (adresses des `shared_ptr`) avec `before`, pour savoir
   quels calques ont changé (`changedLayers`, `voxelsChanged`, `paramsChanged`) ;
4. stocke un `Snapshot` : nom, type, état, durée, changements.

Le viewer utilise ces changements pour afficher par défaut ce que l'étape a produit.

Le **pipeline par défaut** est défini en code dans
[BuiltinStages.cpp](../src/wg/gen/stages/BuiltinStages.cpp) (`parameters`, `island_init`, `erosion`, `upscale`, `hydrology`, `postprocess`, `voxelize`). Une
clé `"pipeline"` dans la config permet de le remplacer, par exemple pour tester une étape
seule.

### 3.3 Déterminisme et seeds

Tout l'aléatoire passe par [Random.hpp](../src/wg/core/Random.hpp) : splitmix64 et FNV-1a, sans
distribution `std::`, pour obtenir le même résultat sur toutes les plateformes. On ne tire
jamais d'une seed globale partagée ; chaque usage dérive sa propre graine à partir d'un nom :

- **paramètre :** `deriveSeed(seed, "param:shape.radius_ratio")` ;
- **usage dans une étape :** `deriveSeed(seed, "shape")`, `deriveSeed(seed, "topo")`… ;
- **sous-usage :** `deriveSeed(deriveSeed(seed, "shape"), "warp_x")`.

Ajouter un tirage quelque part ne décale donc jamais les autres : même seed et même config
donnent un résultat identique au bit près. Un test le vérifie.

### 3.4 Suivi en direct et annulation

Une étape longue appelle `context.progress(fraction)`. Ce que fait le pipeline à cet appel :

- **limitation :** il agit au plus toutes les `previewInterval` secondes (0,1 s dans le
  viewer) ;
- **aperçu :** il prend une copie profonde de l'état (`WorldState::deepCopy`) et l'envoie au
  callback `onStageProgress`. La copie est profonde parce que l'étape continue d'écrire dans
  ses références ;
- **annulation :** il vérifie `shouldCancel`, et lève `GenerationCancelled` si besoin.

Personne n'écoute dans `wgen` : l'appel ne coûte alors presque rien.

### 3.5 Affichage des calques

Chaque calque porte un `LayerMeta` ([LayerMeta.hpp](../src/wg/core/LayerMeta.hpp)) qui dit
comment le colorer ([MapRenderer.cpp](../src/wg/render/MapRenderer.cpp), `LayerPalette`) :

| Style | Rendu |
|-------|-------|
| `Height` | teintes d'altitude au-dessus de la mer, bleu selon la profondeur en dessous, ombrage du relief |
| `Scalar` | niveaux de gris du min au max |
| `Diverging` | bleu (négatif) / blanc (0) / rouge (positif), chaque signe normalisé à son extrême |
| `Categorical` | une couleur par catégorie (matériaux : `materialColor`), légende dans le viewer |

La touche `C` du viewer force les niveaux de gris pour tous les calques.

## 4. Les applications

### `wgen` ([apps/wgen/main.cpp](../apps/wgen/main.cpp))

Il charge la config, exécute le pipeline et écrit :
- le monde (`.wgw`, voir [WorldFile.hpp](../src/wg/io/WorldFile.hpp)) ;
- en option, les paramètres résolus (`--params`), une carte PNG (`--png`) et une carte par
  étape (`--stages`).

Le mode **galerie** (`--gallery sheet.png`, [Gallery.cpp](../apps/wgen/Gallery.cpp)) génère
`--count` seeds en parallèle, une par cœur. Les options :
- `--origin TYPE` force l'origine, et `--origin all` produit un groupe par origine ;
- `--resolution` réduit la grille pour aller plus vite ;
- `--before-after` place chaque île avant et après l'érosion côte à côte.

Les vignettes ne montrent que les hauteurs érodées : la galerie s'arrête après l'érosion (sauf
si la config donne sa propre clé `pipeline`).

Le résultat est une planche PNG de vignettes en hillshade, légendées avec la seed et
l'origine. Elle sert à juger d'un coup d'œil la variété des îles.

### `wview` ([apps/wview/](../apps/wview/))

| Pièce | Rôle |
|-------|------|
| `GenerationJob` | exécute le pipeline dans un thread. Les callbacks remplissent sous mutex une liste de `StageSlot` (statut, progression, snapshot final ou aperçu), que l'UI recopie à chaque frame (`poll`). |
| `Viewer` (main.cpp) | liste des étapes (suit l'étape en cours), liste des calques, vue paramètres, 2D (texture) / 3D (maillage). Surveille la date de modification de la config et relance la génération (l'ancienne est annulée). |
| Cache GPU | une texture ou un maillage par (donnée, mode, palette). Chaque entrée garde un `shared_ptr` sur sa donnée, et elle est libérée quand plus aucun snapshot ne référence cette donnée. |
| Coupe verticale | `X` ouvre un panneau qui montre la roche en place (`renderCrossSection`, lib `render/CrossSection`) le long d'une ligne tracée sur la carte. `S` place le centre de la ligne sous la souris, `Q` / `E` la tournent, la molette règle l'exagération verticale, cadrée automatiquement à l'ouverture. Le survol affiche le matériau. |
| `Meshes` | relief 3D d'un calque (coloré par le calque affiché, relief tiré de `h`), et maillage de voxels avec niveau de détail. |

## 5. Ajouter une étape

1. Créer `src/wg/gen/stages/MonEtape.{hpp,cpp}`, une classe dérivée de `wg::Stage`.
2. Dans `run()` :
   - lire les paramètres avec `world.params().values` ;
   - lire et écrire les calques avec les noms de [IslandLayers.hpp](../src/wg/island/IslandLayers.hpp), en ajoutant les nouveaux noms à ce fichier ;
   - appeler `context.progress()` dans les boucles longues.
3. L'enregistrer dans `BuiltinStages.cpp` (`r.add<MonEtape>("mon_etape")`) et l'ajouter à
   `defaultPipeline()` à sa place.
4. Ajouter le `.cpp` à [src/CMakeLists.txt](../src/CMakeLists.txt).
5. Si l'étape a besoin de nouveaux paramètres, voir
   [Étape 1, § Ajouter un paramètre](etape-1-parametres.md#6-ajouter-un-paramètre).

## 6. Tests

`ctest --test-dir build` lance [tests/main.cpp](../tests/main.cpp). Les tests couvrent :
- **config :** résolution, overrides, listes et validation ;
- **pipeline :** snapshots, copy-on-write, callbacks live et annulation ;
- **étape 2 :** déterminisme au bit près, absence de NaN, bord `FIXED`, île entourée d'eau sur 20 seeds, strates ;
- **stockage :** voxels et format `.wgw`.

Les tests définissent leurs propres étapes simples (`flat`, `add`, `stone`) pour tester le
pipeline indépendamment des étapes réelles.
