# Étapes 4 et 5 : upscale (`upscale`) et hydrologie (`hydrology`)

Tracer les rivières et les lacs sur le terrain final, bloc par bloc. Les rivières naissent en
montagne, d'un bloc de large, là où peu d'eau s'est encore rassemblée, puis se rejoignent en
cours d'eau de plus en plus larges jusqu'à la mer ; les vraies cuvettes deviennent des lacs.

← [Étape 3 : érosion](etape-3-erosion.md) · [Étape 6 : voxelisation](etape-6-voxelisation.md) ·
[Vue d'ensemble](README.md)

Code : [UpscaleStage.cpp](../src/wg/gen/stages/UpscaleStage.cpp),
[HydrologyStage.cpp](../src/wg/gen/stages/HydrologyStage.cpp).

---

## 1. Pourquoi aux blocs

Une première version traçait les rivières sur la grille de simulation (1024²) puis les
redessinait aux blocs en interpolant. Le terrain interpolé et le niveau d'eau interpolé ne
coïncidaient pas toujours : certaines rivières n'atteignaient pas la mer, l'eau dépassait
parfois ses berges (des murs d'eau dans Minecraft), et une rivière faisait au moins 2 blocs
de large. Désormais le terrain est d'abord agrandi aux blocs, et l'hydrologie travaille
directement sur lui : le niveau de l'eau et le terrain sont la même donnée.

## 2. Upscale (étape 4)

`h` passe de la grille de simulation à la résolution des blocs (une cellule par colonne), par
interpolation **bicubique (Catmull-Rom)** : lisse, sans les facettes du bilinéaire, et qui
passe par les valeurs de la grille (`sampleCubic` dans [Grid2D.hpp](../src/wg/core/Grid2D.hpp)).
Le calque `h` est **remplacé** ; `h_initial` et les calques de l'érosion restent sur la grille
de simulation. Environ 160 ms en 4096².

## 3. Hydrologie (étape 5)

### 3.1 La mer

La mer, ce sont les cellules sous le niveau de la mer **reliées au bord de la carte** (l'océan,
en 8-connexité comme l'écoulement). Un creux à l'intérieur des terres qui descend sous le
niveau de la mer (par exemple derrière un tas de sédiments au pied des montagnes) n'est pas
la mer : c'est une dépression comme une autre, creusée vers l'océan ou remplie. Auparavant,
ces flaques servaient d'exutoires et des rivières s'y arrêtaient (sur la seed 7, 11 229
cellules de rivière sur 17 894 n'atteignaient pas l'océan). La mer est recalculée après le
creusement : une flaque drainée par un chenal vers l'océan devient un petit estuaire.

### 3.2 Conditionnement : remplir ou creuser

**Priority-Flood avec creusement hybride** (Lindsay 2016). On inonde depuis les exutoires (la
mer et le bord de la carte), la cellule la plus basse d'abord. Quand l'inondation atteint une
cellule plus basse que celle d'où elle vient, cette cellule est au fond d'une dépression :

- **creuser** : si un chenal le long du chemin de l'inondation (vers l'exutoire) suffit à la
  drainer, en restant dans `breach_max_depth_m` de profondeur et `breach_max_length_m` de
  longueur, il est creusé dans le terrain, chaque cellule 1 mm sous la précédente. C'est ce qui
  ouvre les petites barrières, comme la butte de sédiments que l'érosion laisse parfois au
  niveau de la mer devant une embouchure ;
- **remplir** sinon : la cellule est relevée à 1 mm au-dessus de sa voisine, et marquée comme
  candidate à un lac. Aucun chenal n'est jamais creusé depuis une cellule remplie : un lac
  reste plat.

Les zones plates reçoivent la même pente de 1 mm par cellule. Toute cellule a ainsi une
voisine strictement plus basse, jusqu'à la mer : l'eau ne peut ni boucler ni stagner.

### 3.3 Écoulement et aire drainée

Chaque cellule s'écoule vers sa voisine de plus grande pente (D8, pente divisée par la
distance). Les pentes sont légèrement perturbées (± 10 %, de façon déterministe selon la
seed), sinon les zones plates et les pentes régulières donneraient de longues lignes droites.
L'aire drainée (pluie uniforme pour l'instant) est cumulée de l'amont vers l'aval.

### 3.4 Lacs

Les cellules remplies forment des composantes connexes. Pour chacune, le niveau du lac est le
**terrain le plus bas de son pourtour** : l'eau ne déborde jamais. Une composante devient un lac
si elle a au moins `lake_min_depth_m` d'eau et `lake_min_area_m2` de surface sous ce niveau.
Sinon, elle est **comblée** jusqu'à son niveau de débordement, comme le feraient des
sédiments : ni creux sec, ni flaque.
Les lacs viennent surtout des cuvettes que l'érosion a gardées en eau ([étape 3, lacs](etape-3-erosion.md)) :
sur la seed 7, les deux caldeiras deviennent des lacs de 2,4 et 2,8 ha. Les dépressions de
moins de `breach_max_depth_m` sont creusées. Cette profondeur se mesure sous le terrain
d'origine : auparavant, chaque coupe se mesurait sous le terrain déjà creusé, et des coupes
successives de moins de 3 m traversaient une caldeira de 27 m de profondeur.

### 3.5 Rivières

Une cellule sèche dont l'aire drainée dépasse `river_min_area_m2` est sur une rivière. Le seuil
fixe la **source** : plus il est bas, plus les rivières naissent haut et plus il y a
d'affluents (sur une île volcanique en 4096² : 74 sources à 15 000 m², 192 à 5 000 m²).

```
largeur    = max(river_min_width_m, river_width_coeff × √(aire drainée en m²))   // 1 km² -> 10 m
profondeur = max(1 m, river_depth_ratio × largeur)                               // au milieu du lit
```

- **Bande** : un disque de la largeur de la rivière autour de chaque cellule du tracé, sur les
  cellules sèches seulement (une bande s'arrête à la mer et aux lacs). Sa surface est la plus
  basse des cellules du tracé qui la couvrent. Une source fait 1 bloc de large.
- **Berges** : la surface de l'eau ne dépasse jamais le terrain d'une voisine sèche, ni le
  niveau de la mer à côté d'elle (elle arrive alors au ras de la mer). Il n'y a donc pas de mur d'eau ; là où le niveau baisse d'un
  bloc à l'autre, l'eau s'écoule en petite cascade, comme dans Minecraft.
- **Berges en pente** : le terrain sec au bord d'une rivière ne monte pas plus raide que
  `river_bank_slope_deg` depuis la surface de l'eau ; ce qui dépasse est abaissé, de la rivière
  vers l'extérieur, jusqu'à retrouver le terrain. Une rivière qui traverse une pente coule ainsi
  dans un petit vallon et non au fond d'une tranchée aux bords droits. Une berge n'est jamais
  abaissée sous la mer ni sous un lac voisin. Après cet abaissement, la surface de l'eau est de
  nouveau plafonnée par les berges (une petite marche là où la rivière descend).
- **Lit** : le terrain est creusé sous la surface, de `profondeur` au milieu du lit et de moins
  en moins vers les bords (profil en creux, au moins 1 m partout), et l'eau le remplit.

### 3.6 Paramètres

| Paramètre | Défaut | Rôle |
|-----------|--------|------|
| `river_min_area_m2` | 15 000 | aire drainée à la source |
| `river_width_coeff` | 0,01 | largeur selon l'aire drainée |
| `river_min_width_m` | 1 | largeur des sources |
| `river_depth_ratio` | 0,4 | profondeur au milieu du lit selon la largeur |
| `river_bank_slope_deg` | 35 | pente maximale des berges depuis l'eau |
| `lake_min_depth_m` | 1 | profondeur minimale d'un lac |
| `lake_min_area_m2` | 1 000 | surface minimale d'un lac |
| `breach_max_depth_m` | 3 | profondeur maximale d'un chenal creusé |
| `breach_max_length_m` | 300 | longueur maximale d'un chenal creusé |

### 3.7 Calques produits (à la résolution des blocs)

| Calque | Contenu |
|--------|---------|
| `h` | modifié : chenaux creusés, lits et berges des rivières |
| `water_surface` | dessus de l'eau (m) : niveau de la mer, d'un lac ou d'une rivière ; le terrain s'il est sec |
| `water` | catégories : sec, mer, lac, rivière |

### 3.8 Vérifications et coût

En 4096², sur une île volcanique et trois îles continentales (dont la seed 7) : **toutes les
cellules de rivière sont reliées à l'océan** (au bord de la carte) par de l'eau, et **aucun
bloc d'eau n'a d'air à côté de lui au-dessus d'un sol sec** (aucun mur d'eau). Sur la seed 7,
aucune berge ne dépasse l'eau de plus d'un bloc (auparavant jusqu'à 4 blocs, à la verticale) ;
l'adoucissement abaisse environ 9 000 cellules, de 6 m au plus. Les quelques
cellules sèches restées sous le niveau de la mer au bord de l'eau (à moins d'un mètre, non
reliées à l'océan) sont remontées au niveau de la mer : des berges au ras de l'eau, pas des
trous. Les mêmes vérifications sont dans les tests. Environ 4,4 s,
pour un pic mémoire de 1,1 Go sur toute la génération.

## 4. Limites connues

- **Pluie uniforme** : le vent et l'humidité (`climate`) pourront la pondérer.
- **Taille du `.wgw`** : `h`, `water_surface` et `water` en 4096² font 64 Mo chacun non
  compressés (650 Mo avec les voxels). Une compression zlib du fichier serait bienvenue.
