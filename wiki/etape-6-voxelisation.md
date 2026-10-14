# Étape 6 : voxelisation (`voxelize`) et export Minecraft

Transformer la carte de hauteurs en blocs, puis écrire le monde au format Minecraft. Une
première version volontairement simple : peu de matériaux, pas encore de végétation ni de
grottes (des étapes viendront s'intercaler avant celle-ci).

← [Étape 5 : hydrologie](etape-5-hydrologie.md) · [Vue d'ensemble](README.md)

Code : [VoxelizeStage.cpp](../src/wg/gen/stages/VoxelizeStage.cpp),
[MinecraftExport.cpp](../src/wg/io/MinecraftExport.cpp), [Nbt.cpp](../src/wg/io/Nbt.cpp),
[apps/wmc](../apps/wmc/main.cpp).

---

## 1. Voxelisation

### Hauteur de chaque colonne

`h` est déjà à la résolution des blocs ([étape 4, upscale](etape-5-hydrologie.md#2-upscale-étape-4)) :
une colonne par cellule, sans interpolation.

```
y   = sea_level_y + (h − niveau de la mer) / block_size_m × vertical_exaggeration
top = floor(y) − 1          // le bloc du dessus : sa face supérieure est à floor(y)
```

C'est la convention de la mer, dont le bloc du dessus (`sea_level_y − 1`) a sa face
supérieure au niveau de la mer : un sol entre 0 et 1 m est au ras de l'eau, comme le montre
le visualiseur (et non un bloc au-dessus). Même règle pour la surface des rivières et des
lacs. Le bloc du dessus est limité à `[min_y + 1, max_y − 1]`. L'étape logue les colonnes limitées : les fonds marins
sous le bas du monde (attendu : −64 par défaut, alors que l'océan descend à plusieurs
centaines de mètres) et les sommets coupés en haut (à corriger avec
`world.vertical_exaggeration`).

### Matériaux

| Où | Bloc |
|----|------|
| `y = min_y` | bedrock |
| sous la surface | stone |
| surface en pente douce (≤ `grass_max_slope_deg`, 40°) | grass, sur `dirt_depth_blocks` (3) de dirt ; dirt sous l'eau |
| surface en pente forte | stone |
| rivières et lacs (`water`) | dirt au fond, eau jusqu'à `water_surface` (voir l'[étape 5](etape-5-hydrologie.md)) |
| la mer (`water` : reliée à l'océan), du fond au niveau de la mer | water |

La pente est mesurée sur `h`, en blocs par bloc.

### Mémoire

Le volume est découpé en sections de 16³ ; une section d'un seul bloc ne coûte rien. Pour
chaque colonne de 16 × 16, tout ce qui est sous la plus basse surface (moins la terre) est
rempli d'un coup en sections uniformes de stone ; seules les sections qui traversent la
surface sont détaillées. Une carte de 4096² se voxelise en environ 1,8 s. Le `.wgw` stocke
les sections détaillées sans compression.

## 2. Export Minecraft (`wmc`, `wgen --minecraft`)

```sh
./build/apps/wmc/wmc output/island_params.wgw output/MonIle      # depuis un monde généré
./build/apps/wgen/wgen island_params.jsonc --minecraft output/MonIle   # ou en fin de génération
```

Le dossier produit est un monde complet de **Minecraft Java Edition** : le copier dans
`.minecraft/saves`. Format Anvil, chunks 1.18+ :

- **`region/r.X.Z.mca`** : 32 × 32 chunks par fichier. En-tête de 1024 positions (secteur de
  4 Kio, nombre de secteurs) et 1024 dates, puis chaque chunk compressé en zlib.
- **Chunk** (NBT) : `DataVersion`, `xPos`/`zPos`, `yPos = −4`, `Status = minecraft:full`,
  `isLightOn = 0` (Minecraft calcule la lumière au chargement), et une `sections` par tranche
  de 16 blocs de −64 à 319. Chaque section a une palette d'états (dans l'ordre d'apparition)
  et les indices empaquetés dans des longs : `max(4, bits)` bits par bloc, sans chevaucher
  deux longs, dans l'ordre (y, z, x) — le même que nos sections. Biome : plaines partout.
- **`level.dat`** (NBT gzip) : mode créatif, commandes autorisées, midi, spawn sur la terre
  la plus proche du centre. Hors de la carte : un monde vide (préréglage « void » du
  générateur plat).
- **Position** : la carte est centrée sur l'origine (x et z de −2048 à 2047 pour 4096 blocs).
- **Version** : `DataVersion 3955` (1.21.1) par défaut. Les versions plus récentes mettent
  le monde à jour en l'ouvrant ; les plus anciennes le refusent (`wmc --data-version`).
- **Sections hors du monde Minecraft** (y < −64 ou ≥ 320) : ignorées, et comptées.

Correspondance des blocs : air, bedrock, stone, dirt, `grass_block`, sand, gravel,
`snow_block`, water.

Les régions sont compressées en parallèle (une par thread) : environ 2 s pour 4096².

### Vérification

Un test écrit un petit monde et vérifie sa structure ; le NBT est vérifié octet par octet.
Les fichiers d'un monde complet ont aussi été relus par un décodeur indépendant (Python,
bibliothèque standard) : `level.dat`, en-têtes de région, palettes et empaquetage, colonne du
spawn (grass sur 3 dirt, puis stone, bedrock en bas) et colonne de mer.
