# World Generator

Générateur de monde **fini** (un continent) en C++20, pensé pour être exporté vers Minecraft
mais indépendant de tout format de jeu.

Comme le monde est généré en entier (et pas chunk par chunk), les étapes peuvent faire des
traitements globaux : érosion, rivières, simulation climatique...

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
# Générer un monde -> output/default.wgw, + carte finale, + une carte par étape
./build/apps/wgen/wgen configs/default.json --png output/default.png --stages output/stages

# Visualiser depuis une config : la génération se suit en direct, chaque étape
# est inspectable, et tout est régénéré à chaque sauvegarde du JSON
./build/apps/wview/wview configs/default.json
# ...ou un monde déjà généré
./build/apps/wview/wview output/default.wgw
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
| `L`                     | Changer la donnée affichée (calque 2D `height`, ..., `voxels`) |
| `C`                     | Palette des calques 2D : `terrain` / `grayscale`                |
| `TAB`                   | Vue 2D (carte) / 3D                                             |
| `R` / `F`               | Régénérer / recadrer la caméra                                  |
| 2D                      | Glisser : déplacer, molette : zoom, survol : valeur sous la souris |
| 3D                      | Clic gauche : orbite, clic droit : déplacer, molette : zoom     |

En 3D, les calques 2D sont affichés en relief et les voxels en cubes (avec un niveau de détail
réduit sur les grands mondes, indiqué dans le panneau).

## Configuration

```jsonc
{
  "seed": 1337,
  "width": 1024,
  "length": 1024,
  "pipeline": [
    { "type": "flat" }
  ]
}
```

| Clé        | Type             | Défaut | Description                                  |
|------------|------------------|--------|----------------------------------------------|
| `seed`     | entier ou texte  | requis | Graine (un texte est hashé, comme Minecraft) |
| `width`    | entier > 0       | requis | Taille en X (blocs)                          |
| `length`   | entier > 0       | requis | Taille en Z (blocs)                          |
| `minY`     | entier           | `-64`  | Y minimum du monde                           |
| `maxY`     | entier           | `320`  | Y maximum (exclu)                            |
| `seaLevel` | entier           | `62`   | Niveau de la mer                             |
| `pipeline` | tableau          | requis | Étapes de génération, exécutées dans l'ordre |

Chaque étape du pipeline est un objet avec un `type`, un `name` optionnel (par défaut le type ;
il doit être unique et sert aussi à dériver la graine de l'étape) et ses paramètres. Les
paramètres inconnus (fautes de frappe) sont signalés comme des erreurs. Les commentaires `//`
sont acceptés.

### Étapes disponibles

| Type   | Entrée → sortie | Paramètres (défaut)                           |
|--------|-----------------|-----------------------------------------------|
| `flat` | → calque        | `layer` (`height`), `value` (`seaLevel + 2`) |

## Architecture

```
src/wg/                    bibliothèque "wg" (aucune dépendance au rendu)
  core/WorldState          état du monde : calques 2D nommés + volume de voxels optionnel
  core/VoxelVolume         blocs 3D en sections 16³ (copy-on-write, sections uniformes compactes)
  core/Grid2D              grille 2D générique
  core/Block               palette de blocs générique (pas de noms Minecraft)
  core/Config              lecture/validation du JSON
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
   - `run(WorldState&, const StageContext&)` lit et écrit les calques ou voxels dont elle a
     besoin (`world.editLayer("height")`, `world.editVoxels()`...). `context.seed` est une graine
     propre à l'étape.
2. L'enregistrer dans `src/wg/gen/stages/BuiltinStages.cpp` : `r.add<MonEtape>("mon_etape");`
3. Ajouter le `.cpp` dans `src/CMakeLists.txt`.
4. L'insérer où on veut dans le `pipeline` du JSON.

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
- [raylib](https://www.raylib.com) : viewer
