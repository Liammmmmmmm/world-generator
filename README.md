# World Generator

Générateur de monde **fini** (un continent) en C++20, pensé pour être exporté vers Minecraft
mais indépendant de tout format de jeu.

Comme le monde est généré en entier (et pas chunk par chunk), les étapes peuvent faire des
traitements globaux, comme l'érosion.

> Pour comprendre le fonctionnement interne (architecture, algorithmes de chaque étape),
> voir le [wiki](wiki/README.md).

## Build

Prérequis : CMake ≥ 3.24 et un compilateur C++20. Les dépendances sont téléchargées
automatiquement par CMake (FetchContent).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # tests unitaires
```

Options CMake : `-DWG_BUILD_VIEWER=OFF` (sans raylib), `-DWG_BUILD_TESTS=OFF`.

## Utilisation

```sh
# Générer un monde -> output/island_params.wgw, + paramètres résolus,
# + carte finale, + une carte par étape
./build/apps/wgen/wgen island_params.jsonc --params output/params.json --png output/map.png --stages output/stages

# Galerie : beaucoup de seeds d'un coup, en vignettes (ici 16 par origine)
./build/apps/wgen/wgen island_params.jsonc --gallery output/gallery.png --origin all --count 16 --resolution 512

# Visualiser depuis une config : la génération se suit en direct, chaque étape
# est inspectable, et tout est régénéré à chaque sauvegarde du fichier
./build/apps/wview/wview island_params.jsonc
# ...ou un monde déjà généré
./build/apps/wview/wview output/island_params.wgw
```

### Viewer

La génération tourne en arrière-plan : la liste affiche toutes les étapes du pipeline (grisées
tant qu'elles n'ont pas commencé, avec un pourcentage pendant qu'elles tournent, leur durée une
fois finies). Tant que la sélection est sur l'étape en cours, elle la suit et affiche son état
en direct (pour les étapes qui rapportent leur progression, voir plus bas). Sélectionner une
autre étape arrête le suivi ; revenir sur l'étape en cours le réactive. Sauvegarder le JSON
annule la génération en cours et en relance une.

### Contrôles du viewer

| Touche                  | Action                                                          |
|-------------------------|-----------------------------------------------------------------|
| `↑` `↓` / `1`-`9` / clic | Choisir l'étape affichée                                        |
| `L`                     | Changer la donnée affichée (`parameters`, calques 2D, `voxels`) |
| `C`                     | Palette : `auto` (selon le type de calque) / `grayscale`        |
| `TAB`                   | Vue 2D (carte) / 3D                                             |
| `R` / `F`               | Régénérer / recadrer la caméra                                  |
| `X`                     | Coupe verticale de la roche (panneau en bas)                    |
| `S` / `Q` `E` / molette | Coupe : placer son centre sous la souris / la tourner / exagération verticale |
| 2D                      | Glisser : déplacer, molette : zoom, survol : valeur sous la souris |
| 3D                      | Clic gauche : orbite, clic droit : déplacer, molette : zoom     |

Palettes `auto` : altitudes en teintes hypsométriques avec ombrage, et mer en bleu selon la
profondeur ; champs signés en divergent bleu / blanc / rouge ; matériaux avec une couleur
chacun et une légende ; autres champs en niveaux de gris. En 3D, le relief vient de `h` et il
est coloré par le calque affiché.

La vue `parameters` liste tous les paramètres résolus, colorés selon leur origine (vert : tiré
de la seed, blanc : fichier de config, gris : défaut fixe). Molette pour défiler.

En 3D, les calques 2D sont affichés en relief et les voxels en cubes (avec un niveau de détail
réduit sur les grands mondes, indiqué dans le panneau).

## Configuration

La config de référence est [`island_params.jsonc`](island_params.jsonc) (JSON avec commentaires
`//`). Elle décrit tous les paramètres du générateur d'île, section par section.

### Résolution des paramètres (étape 1)

**Convention :** `null` = tiré de la seed, toute autre valeur override le tirage.

- **Un flux aléatoire par paramètre**, dérivé de la seed et du chemin du paramètre
  (`shape.radius_ratio`, `geology.strata[2].thickness_m`...). La valeur est toujours tirée,
  puis remplacée par celle du fichier si elle n'est pas `null`. Overrider un paramètre, ou en
  ajouter un nouveau dans le code, ne décale donc jamais la valeur des autres.
- **Dépendances.** Certaines distributions dépendent de paramètres déjà résolus, surtout
  `origin.type` : forme de l'île, matériaux des strates, mode de topographie initiale,
  probabilité de plissement... Elles utilisent la valeur finale, override comprise : forcer
  `origin.type: "volcanic"` donne des strates de basalte et de tuf, avec les mêmes tirages.
- **Listes** (`geology.strata`). Si le fichier donne un tableau, sa longueur fixe le nombre
  d'éléments. Les champs absents ou `null` d'un élément sont tirés comme les autres ;
  `{ "material": "granite" }` fixe le matériau et tire l'épaisseur. Sinon, le nombre vient de
  `*_count`, tiré ou fixé.
- **Paramètres sans aléatoire** (`world.*`, `simulation.resolution`...) : la valeur du fichier, ou
  un défaut fixe si elle est `null`.
- **Validation.** Les types, les choix possibles (`origin.type`, matériaux connus de
  `geology.materials`...) et les clés inconnues (fautes de frappe) sont vérifiés au chargement,
  avec le chemin du paramètre fautif.

Les distributions sont toutes dans [`IslandParams.cpp`](src/wg/params/IslandParams.cpp). Les
plages notées `[a, b]` dans le fichier y sont reprises telles quelles ; les autres sont des
choix plausibles, à ajuster. `wgen --params out.json` écrit la config complète, avec toutes les
valeurs remplies : réutilisée comme config, elle redonne exactement les mêmes paramètres.

Unités : mètres et degrés. Les fréquences de bruit sont en cycles par rayon d'île, et les
coordonnées (`x`, `y`, `center`) en mètres depuis le centre de la carte. `world.min_y` et
`world.max_y` (défauts `-64` / `320`) bornent le monde voxel ; ils sont provisoires, à revoir
avec la voxelisation.

### Pipeline

Sans clé `pipeline`, le générateur exécute le pipeline d'île par défaut
(`StageRegistry::defaultPipeline()` dans `BuiltinStages.cpp`), complété au fur et à mesure des
étapes. Une clé `pipeline` permet de le remplacer, pour tester une étape isolément :

```jsonc
"pipeline": [ { "type": "parameters" }, { "type": "island_init" } ]
```

Chaque entrée a un `type`, un `name` optionnel (unique, sert aussi à dériver la graine de
l'étape) et les paramètres propres à l'étape.

### Étapes disponibles

| Type          | Rôle                                                                                          |
|---------------|-----------------------------------------------------------------------------------------------|
| `parameters`  | Étape 1 : publie les paramètres résolus dans l'état du monde (`world.params()`)                |
| `island_init` | Étape 2 : état initial de l'île sur la grille de simulation, sans érosion (voir ci-dessous)    |

#### `island_init`

Grille de `simulation.resolution` cellules de large, cellules carrées, origine au centre.
Avec `R` le rayon de l'île et `q = p / R`, toutes les fréquences de bruit sont en cycles par
rayon. Chaque usage aléatoire a sa sous-seed (`"shape"`, `"topo"`, `"seafloor"`). Le
résultat est identique au bit près pour une même seed et une même config.

| Calque               | Contenu                                                                         |
|----------------------|---------------------------------------------------------------------------------|
| `h`                  | altitude (m), mer à `world.sea_level_y`                                         |
| `mask`               | terre [0, 1] : smoothstep de `coast_dist` sur `shape.coast_falloff_m`           |
| `coast_dist`         | distance signée à la côte (m, > 0 = terre) : lobes + domain warp fBm            |
| `surface_material`   | matériau à la surface (`material(x, y, h)`)                                     |
| `lagoon`             | 1 dans le lagon d'un atoll                                                      |
| `fold_offset`        | déplacement vertical des strates par le pli (m)                                 |
| `h_initial`          | copie de `h` en fin d'étape                                                     |
| `flags` (octets)     | bit `FIXED` sur l'anneau de bord, où `h = boundary_depth_m`                     |

Le détail des algorithmes est dans [wiki/etape-2-initialisation.md](wiki/etape-2-initialisation.md).


## Architecture

```
src/wg/                    bibliothèque "wg" (aucune dépendance au rendu)
  core/WorldState          état du monde : paramètres résolus, calques 2D nommés, voxels
  core/VoxelVolume         blocs 3D en sections 16³ (copy-on-write, sections uniformes compactes)
  core/Grid2D              grille 2D générique
  core/Block               palette de blocs générique (pas de noms Minecraft)
  core/Config              chargement de la config (seed + paramètres résolus)
  params/IslandParams      structure typée de tous les paramètres + leurs distributions
  params/ParamResolver     tirage par paramètre depuis la seed, overrides, validation
  island/IslandLayers      noms des calques de l'île et flags de cellule
  island/IslandGrid        géométrie de la grille de simulation (origine au centre)
  island/Geology           strates et plis : material(x, y, z)
  noise/Fbm                fBm OpenSimplex2 (FastNoiseLite)
  core/Random              RNG déterministe et dérivation de graines
  gen/Stage                interface d'une étape
  gen/StageParams          accès typé et validé aux paramètres d'une étape
  gen/StageRegistry        type (string) -> fabrique d'étape
  gen/Pipeline             construit le pipeline depuis la config, l'exécute, garde un snapshot
                           par étape, notifie début / progression / fin (callbacks, annulation)
  gen/stages/              les étapes ; BuiltinStages.cpp les enregistre
  io/WorldFile             format natif binaire .wgw
  io/PngExport             écriture PNG
  render/MapRenderer       vues, palettes, rendu 2D (partagé par le PNG et le viewer)
apps/wgen                  CLI
apps/wview                 viewer raylib 2D/3D, génération dans un thread (GenerationJob)
tests/                     tests unitaires
```

### Calques et voxels

Le monde passe par deux représentations :

- **Calques 2D** (`Grid2D<float>` nommés : `height`, plus tard `moisture`, `flow`...). C'est là
  que travaillent le bruit et l'érosion.
- **Volume de voxels**, construit à partir des calques par une étape de voxelisation, puis
  modifié par les étapes 3D (grottes, props, structures).

Les deux coexistent : après la voxelisation, les calques restent disponibles pour les étapes
suivantes (ex. placer des arbres selon `moisture`).

### Snapshots

Après chaque étape, le pipeline garde une copie de l'état (`Snapshot`). Les données sont
partagées en copy-on-write : une étape qui modifie `height` en obtient une copie privée, et
les calques non modifiés restent partagés. Côté voxels, seules les sections 16³ touchées sont
dupliquées. Garder toutes les étapes coûte donc seulement ce que chaque étape a changé. Le
pipeline note aussi ce que chaque étape a modifié, ce qui permet au viewer d'afficher par
défaut la bonne donnée.

### Suivi en direct d'une étape

Une étape longue appelle `context.progress(fraction)` régulièrement (par exemple à chaque
itération d'érosion) :

```cpp
void ErosionStage::run(WorldState& world, const StageContext& context) {
    auto& height = world.editLayer("height");
    for (int i = 0; i < iterations_; ++i) {
        simulateDroplets(height);
        context.progress(float(i + 1) / iterations_);
    }
}
```

L'appel est limité en fréquence (10 fois par seconde dans le viewer) et ne coûte rien quand
personne n'écoute (`wgen`). Quand le viewer écoute, le pipeline prend une copie isolée de l'état
(`WorldState::deepCopy`). L'étape peut donc garder ses références (`height` ci-dessus) et
continuer à écrire pendant que le viewer affiche la copie. `progress()` est aussi l'endroit où
une génération annulée s'arrête : une étape qui ne l'appelle jamais ne peut être interrompue
qu'à sa fin.

### Ajouter une étape

1. Créer `src/wg/gen/stages/MonEtape.hpp/.cpp`, une classe dérivée de `wg::Stage` :
   - le constructeur lit ses paramètres via `StageParams` (`params.get("strength", 1.0f)`) ;
   - `run(WorldState&, const StageContext&)` lit les paramètres (`world.params().values`, par
     ex. `.shape.radiusRatio`), puis lit et écrit les calques ou voxels dont elle a besoin
     (`world.editLayer("height")`, `world.editVoxels()`...). `context.seed` est une graine propre
     à l'étape.
2. L'enregistrer dans `src/wg/gen/stages/BuiltinStages.cpp` (`r.add<MonEtape>("mon_etape");`)
   et l'ajouter à `defaultPipeline()` à sa place.
3. Ajouter le `.cpp` dans `src/CMakeLists.txt`.

Les étapes ne se connaissent pas entre elles : elles ne partagent que des noms de calques.
Insérer une étape `erosion` entre une étape de bruit et la voxelisation ne demande donc de
toucher à aucune autre étape.

### Export vers d'autres formats

Les exporteurs liront un `WorldState` et convertiront la palette `wg::Block` vers leurs propres
identifiants. Minecraft (format Anvil : régions `.mca`, NBT compressé zlib) sera un exporteur
parmi d'autres, au même titre que le format de ton clone. Les sections 16³ du `VoxelVolume`
correspondent directement aux sections Minecraft.

## Dépendances

- [nlohmann/json](https://github.com/nlohmann/json) : configuration
- [stb_image_write](https://github.com/nothings/stb) : export PNG
- [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) : bruit OpenSimplex2 / fBm
- [raylib](https://www.raylib.com) : viewer
