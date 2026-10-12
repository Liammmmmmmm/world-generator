# Étape 4 : hydrologie (`hydrology`)

Tracer les rivières et les lacs sur le relief érodé, avant la voxelisation. Les rivières
naissent en montagne, là où peu d'eau s'est encore rassemblée, puis se rejoignent en cours
d'eau de plus en plus larges jusqu'à la mer ; les lacs remplissent les creux.

← [Étape 3 : érosion](etape-3-erosion.md) · [Étape 6 : voxelisation](etape-6-voxelisation.md) ·
[Vue d'ensemble](README.md)

Code : [HydrologyStage.cpp](../src/wg/gen/stages/HydrologyStage.cpp). Le dessin aux blocs est
dans [VoxelizeStage.cpp](../src/wg/gen/stages/VoxelizeStage.cpp).

---

## 1. Algorithme (sur la grille de simulation)

1. **Remplissage des dépressions** (Priority-Flood, Barnes et al. 2014). On part des exutoires
   (la mer et le bord de la carte), et on inonde de proche en proche, la cellule la plus basse
   d'abord. Une cellule atteinte depuis une voisine plus haute est relevée à son niveau, plus
   1e−4 m. Le résultat est la **surface de l'eau** (`water_surface`) : le terrain, sauf dans
   les creux, remplis jusqu'à leur point de débordement avec une pente infime vers leur
   sortie.
2. **Directions d'écoulement** (D8) : chaque cellule s'écoule vers la voisine de plus grande
   pente sur cette surface (pente divisée par la distance, √2 en diagonale). Grâce au
   remplissage, toute cellule a une voisine plus basse : l'eau traverse les lacs et finit
   toujours à la mer, sans boucle ni cuvette.
3. **Aire drainée** : chaque cellule apporte sa surface (pluie uniforme pour l'instant ; le
   climat pourra la pondérer), cumulée vers l'aval, des cellules les plus hautes aux plus
   basses.
4. **Lacs** : les cellules sous au moins `lake_min_depth_m` d'eau (surface − terrain),
   regroupées en composantes connexes, gardées si elles couvrent au moins
   `lake_min_area_m2`. Profondeur dans `lake_depth`.
5. **Rivières** : une cellule hors mer dont l'aire drainée dépasse `river_min_area_m2` porte
   une rivière. Le seuil fixe la **source** : plus il est bas, plus les rivières naissent haut
   et plus il y a d'affluents. Elle s'élargit à chaque confluence :
   ```
   largeur    = max(river_min_width_m, river_width_coeff × √(aire drainée en m²))   // 1 km² -> 10 m
   profondeur = max(1 m, river_depth_ratio × largeur)
   ```

Environ 170 ms en 1024².

| Paramètre | Défaut | Rôle |
|-----------|--------|------|
| `river_min_area_m2` | 15 000 | aire drainée à la source (sur une île volcanique en 1024² : environ 4 500 cellules de rivière) |
| `river_width_coeff` | 0,01 | largeur selon l'aire drainée |
| `river_min_width_m` | 2 | largeur des sources |
| `river_depth_ratio` | 0,15 | profondeur selon la largeur |
| `lake_min_depth_m` | 0,5 | profondeur minimale d'un lac |
| `lake_min_area_m2` | 1 000 | surface minimale d'un lac |

## 2. Calques produits

| Calque | Contenu |
|--------|---------|
| `water_surface` | terrain avec ses dépressions remplies : le niveau des lacs (m) |
| `lake_depth` | profondeur d'eau des lacs (m), 0 ailleurs |
| `drainage` | log10 de l'aire drainée (m²) : le réseau complet, même sous le seuil |
| `river_width`, `river_depth` | largeur et profondeur de la rivière (m), 0 hors rivière |
| `flow_direction` (octets) | direction D8 de chaque cellule (0 à 7, voir `kFlowDi`/`kFlowDj` dans [IslandLayers.hpp](../src/wg/island/IslandLayers.hpp)), 255 aux exutoires |

## 3. Aux blocs

La voxelisation dessine chaque tronçon de rivière (du centre d'une cellule à celui de sa
cellule aval) comme une bande aussi large que la rivière. La surface de l'eau y varie
linéairement d'un niveau à l'autre ; là où des bandes se recouvrent, la surface la plus
basse et le lit le plus profond l'emportent. Le lit est creusé sous la surface de l'eau, de
la profondeur de la rivière, et rempli d'eau. Les lacs sont remplis jusqu'à leur niveau là où
leur profondeur interpolée dépasse 25 cm.

## 4. Limites connues

- **Tracés en ligne droite** sur les zones les plus plates (dépressions remplies) : c'est
  l'artefact classique du D8, qui n'a que 8 directions. Une direction pondérée (D∞) ou un
  lissage des tracés corrigerait cela.
- **Rivières en tranchée** : le lit est creusé à la surface de l'eau, ce qui peut laisser une
  berge verticale de quelques blocs là où le terrain interpolé est plus haut que la rivière.
- **Les ravines des volcans** sont de petits bassins radiaux : peu atteignent le seuil d'une
  source.
- **Pluie uniforme** : le vent et l'humidité (`climate`) pourront la pondérer.
