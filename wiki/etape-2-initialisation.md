# Étape 2 : initialisation de l'île (`island_init`)

Construire l'**état initial** de l'île sur la grille de simulation : contour, fonds marins, relief de départ, motifs tectoniques et géologie. L'étape ne fait **aucune simulation** (pas d'érosion, pas de boucle temporelle) : c'est l'étape 3 qui fera vivre cet état.

← [Étape 1](etape-1-parametres.md) · [Vue d'ensemble](README.md)

Code : [IslandInitStage.cpp](../src/wg/gen/stages/IslandInitStage.cpp), avec
[island/](../src/wg/island/) pour la grille, les noms de calques et la géologie.

---

## 1. Conventions

### Grille ([IslandGrid.hpp](../src/wg/island/IslandGrid.hpp))

```
nx    = simulation.coarse_resolution          (1024)
cellM = world.width · block_size / nx         (4 m pour une carte de 4096 blocs)
ny    = arrondi(world.length · block_size / cellM)
centre de la cellule (i, j) :  x = (i + 0.5 − nx/2) · cellM     origine au centre de la carte
                               y = (j + 0.5 − ny/2) · cellM
```

### Rayon et coordonnées normalisées

- **Rayon :** `R = shape.radius_ratio × demi-taille de la carte` (`IslandParams::islandRadiusM`).
- **Coordonnées normalisées :** `q = p / R`.
- **Bruits :** on les évalue en `q`, et leurs fréquences sont donc en **cycles par rayon**. Le
  résultat a la même allure à 256² ou à 1024².

### Altitudes

`h` est une altitude **absolue** en mètres, avec la mer à `info.seaLevelM()`. Les profondeurs
et élévations de la config sont relatives à la mer, et on y ajoute le niveau de la mer
(`sea`).

### Sous-seeds

Chaque usage aléatoire a la sienne :

| Usage | Graine |
|-------|--------|
| lobes | `deriveSeed(deriveSeed(seed, "shape"), "lobes")` |
| baies | `deriveSeed(shapeSeed, "bays")` |
| passes et largeur de l'anneau | `deriveSeed(shapeSeed, "ring_passes")`, `"ring_width"` |
| warp X / Y | `deriveSeed(shapeSeed, "warp_x")`, `"warp_y"` |
| relief initial | `deriveSeed(seed, "topo")` |
| micro-perturbation | `deriveSeed(topoSeed, "jitter")` |
| fond marin | `deriveSeed(seed, "seafloor")` |

### Bruit ([noise/Fbm.hpp](../src/wg/noise/Fbm.hpp))

FastNoiseLite en OpenSimplex2, sommé en fBm sur `octaves` couches (fréquence × `lacunarity`,
amplitude × `gain` à chaque couche). La sortie est à peu près dans [−1, 1].

## 2. Calques produits

Tous les calques font `nx × ny` cellules. Les noms sont définis dans
[IslandLayers.hpp](../src/wg/island/IslandLayers.hpp), qui sert de contrat avec l'étape 3.

| Calque | Type / style | Contenu |
|--------|--------------|---------|
| `h` | float, Height | altitude (m) |
| `mask` | float, Scalar | terre ∈ [0, 1] |
| `coast_dist` | float, Diverging | distance signée à la côte (m), > 0 = terre |
| `lagoon` | float, Scalar | 1 dans le lagon d'un atoll, sinon 0 |
| `uplift_pattern_k` | float, Diverging | motif spatial de la phase tectonique k, ∈ [−1, 1] |
| `surface_material` | float, Categorical | id du matériau en surface (étiquettes = noms des matériaux) |
| `cum_uplift` | float | soulèvement cumulé, 0 |
| `sediment` | float | épaisseur de sédiments, 0 |
| `ice` | float | épaisseur de glace, 0 |
| `fold_offset` | float, Diverging | déplacement vertical des strates par le pli (m) |
| `fault_traces` | float, Scalar | 1 sur la trace d'une faille (validation) |
| `flags` | octets | bit `kFixed` sur l'anneau de bord, `kInLagoon` dans un lagon |
| `h_initial` | float, Height | copie de `h` en fin d'étape |

Tous les calques sont créés dès le début de l'étape, dans l'ordre d'affichage. Le viewer en
suit donc le remplissage en direct.

## 3. Algorithme, sous-étape par sous-étape

### 3.1 Masque terre/mer (`shape.*`)

Le contour vient d'une **distance signée** `s` : positive sur terre, négative en mer, nulle
sur la côte.

**Repère de forme.** Toute la forme est construite dans un repère aligné sur l'axe principal
(`main_axis_deg`) et comprimé le long de cet axe par l'élongation :
`(U, V) = (u / elongation, v)`. Un lobe rond dans ce repère est donc, dans le monde, une
forme étirée le long de l'axe. C'est ce qui donne les îles longues (sédimentaires) ou
allongées (continentales).

**a. Lobes elliptiques.** On tire `lobe_count` ellipses dans le repère de forme. Chaque lobe
consomme toujours 5 tirages du flux `"lobes"`.

```
centre    : point uniforme du disque unité (√u), × R · lobe_spread
taille    : rayon moyen r = R · U[lobe_radius_min, lobe_radius_max]
forme     : allongement e = U[1, lobe_elongation_max] → demi-axes a = r·√e, b = r/√e (aire conservée)
direction : (1 − lobe_alignment) · U[−90°, 90°] par rapport à l'axe
```

Les lobes sont ensuite **remis à l'échelle** pour que l'île tienne dans `R`, sa demi-longueur
le long de l'axe : `k = R / (elongation · max_i(|c_i| + max(a_i, b_i)))`.

Ce que chaque paramètre change sur la forme :

| Paramètre | Effet |
|-----------|-------|
| `lobe_radius_*` petit devant `lobe_spread` | lobes distincts : presqu'îles, isthmes, îles multiples |
| `lobe_radius_*` grand | les lobes fusionnent en une seule masse |
| `lobe_elongation_max` | lobes en ellipses : îles allongées, croissants |
| `elongation` | toute la forme étirée le long de l'axe : île allongée, longue île fine |
| `lobe_alignment` | 1 = lobes parallèles à l'axe (île « cigare »), 0 = orientations libres |
| `lobe_blend` | union adoucie qui comble les creux entre lobes |

**Distance à une ellipse.** On utilise l'approximation au premier ordre `f / |∇f|` de la
fonction implicite `f = |(u/a, v/b)| − 1`, avec `(u, v)` les coordonnées dans le repère du lobe.
Elle est exacte le long des deux axes et suffisante ailleurs pour une côte
(`Lobe::signedDistance`). À l'intérieur, elle est plafonnée au demi-petit-axe `min(a, b)`, la
plus grande distance au bord qui existe dans une ellipse. Sans ce plafond, elle tend vers `a`
le long du grand axe et vers `b` en travers : la discontinuité au centre faisait croire à une
côte, et la rampe côtière creusait le terrain au milieu de l'île.

**Union** des lobes : `s = smoothMax(s, s_i, lobe_blend · R / elongation)`. Avec un
`lobe_blend` nul, c'est un simple `max`.

**Retour en mètres.** Une distance mesurée dans le repère de forme est fausse le long de
l'axe : elle y est divisée par l'élongation. Or la largeur du plateau et du talus doit être en
mètres partout. On calcule donc d'abord la distance aux lobes sur toute la grille (passe 1),
puis on la divise par la norme de son gradient dans le monde, calculée par différences finies
(passe 2) :

```
s = s_forme / clamp(|∇ s_forme|, 1 / elongation, 1)
```

C'est encore l'approximation `f / |∇f|`. La norme mesurée est bornée à
`[1 / elongation, 1]`, l'intervalle exact pour un étirement pur. En dehors, ce n'est que le
bruit du warp, et pour une île ronde la renormalisation ne fait alors plus rien. Les baies et la
contrainte de bord sont appliquées ensuite, directement en mètres.

**Baies (lobes soustractifs).** `bay_count` vaut 0, 1 ou 2, avec des probabilités qui
dépendent de l'origine (50 / 35 / 15 % pour une île continentale, jamais pour un atoll).
Chaque baie est un disque de rayon `R · U[bay_radius_min, bay_radius_max]`, placé sur le
contour d'un lobe tiré au hasard, puis poussé vers l'intérieur des terres de
`rayon · (2 · bay_penetration − 1)`. À 0,5, la baie est centrée sur la côte ; plus haut, elle
mord davantage. Elle est soustraite par `s = smoothMin(s, |p' − c_b| − r_b, coast_falloff_m)`.
Quand une baie tombe à l'intérieur d'un autre lobe, elle crée un lagon fermé : c'est rare.

**b. Domain warp.** On déforme la **position** avant de mesurer la distance (pas la valeur
après), ce qui découpe la côte en caps et en baies au lieu de la rendre juste bosselée :

```
p' = p + (R / elongation) · noise.amplitude · ( fbm_x(q), fbm_y(q) )      deux bruits décorrélés
```

L'amplitude est relative à la **demi-largeur** de l'île (`R / elongation`) : relative à `R`,
elle déchiquetait les îles fines.

**c. Distance signée** : l'union des lobes moins les baies (voir a), évaluée en `p'`.

**c bis. Mode `ring` (atoll, `shape.mode`).** Les îles coralliennes utilisent ce mode par
défaut. La distance au contour `s_disk`, calculée comme ci-dessus (lobes, warp, baies), sert
de guide à un **récif annulaire** :

```
inward = s_disk − ring_offset_m                 // > 0 : à l'intérieur de l'axe de l'anneau
w      = ring_width_m · (n − q) / (moyenne − q) · (1 + ring_width_noise · m)
s      = w / 2 − |inward|
```

- **`n`, les passes.** C'est un bruit (flux `"ring_passes"`), et `q` est son quantile
  `ring_pass_fraction`, mesuré le long de l'axe de l'anneau. Exactement cette part de
  l'anneau a une largeur négative : ce sont les **passes**, qui découpent l'anneau en
  **motus**. Le terme `/ (moyenne − q)` garde `ring_width_m` comme largeur moyenne.
- **`m`, la texture de largeur.** C'est un second bruit indépendant (flux `"ring_width"`), qui
  fait seulement varier la largeur. Le facteur reste positif, donc il ne déplace pas les passes.
- **Réglages des deux bruits.** Ils utilisent `ring_noise_frequency` et `ring_noise_octaves`
  (1 à 3 : quelques passes larges plutôt qu'une multitude de petites).
- **Lagon.** C'est l'eau à l'intérieur de l'anneau (`inward > 0` et `s < 0`). Il est marqué dans
  le calque `lagoon` (0/1) et par le bit `kInLagoon` de `flags`. En attendant la topographie
  `atoll` (jalon C), son fond reste à la profondeur du plateau au lieu de plonger comme le
  large.

Note : `ring_offset_m` est compté **vers l'intérieur** (retrait par rapport au contour), d'où
`s_disk − ring_offset_m`. Quand la forme est plus étroite que l'anneau, il n'y a pas de place
pour un lagon à cet endroit, et l'anneau devient une île pleine.

**d. Contrainte de bord de carte.** Le bord doit toujours être précédé de trois bandes : le
côté mer de la transition de la côte, le plateau entier, puis la pente entière. Ainsi, le fond
atteint exactement `boundary_depth_m` à l'anneau `FIXED`, sans marche :

```
anneau = coast_falloff_m/2 + shelf_width_m + talus_width_m
s      = smoothMin( s, distance_au_bord − anneau, coast_falloff_m )
```

`smoothMin` est un minimum polynomial adouci, qui évite un pli dans `s`. Avec les paramètres
par défaut, l'étape 1 a déjà laissé la place nécessaire (voir
[§ 3.5 de l'étape 1](etape-1-parametres.md#35-bathymétrie-qui-tient-dans-la-carte)). La
contrainte ne grignote l'île que si la config force un plateau ou une pente trop grands.

**e. Masque :** `mask = smoothstep(−coast_falloff_m/2, +coast_falloff_m/2, s)`. `s` est stocké
dans `coast_dist`.

### 3.2 Bathymétrie (`shape.shelf_*`, `talus_*`, `seafloor_noise`, `simulation.boundary_depth_m`)

Avec `d = −s`, la distance au large de la côte :

```
d ≤ shelf_width           →  plateau :  h_sea = shelf_depth
d > shelf_width           →  t = smoothstep(0, talus_width, d − shelf_width)
                              h_sea = lerp(shelf_depth, boundary_depth, t)
                                    + 4t(1−t) · seafloor_noise.amplitude · bruit(q)
h_sea = min(h_sea, shelf_depth)       // le fond ne remonte jamais au-dessus du plateau
```

Le bruit du fond (une seule octave, basse fréquence) est pondéré par `4t(1−t)` : il est nul
au bord du plateau, nul au fond, et maximal à mi-pente. Ainsi il ne crée pas de marche contre
l'anneau `FIXED`, ni d'îlot sur le plateau.

### 3.3 Relief initial (`initial_topography.*`)

Le mode dépend de l'origine (volcanique → `volcanic_cone`, continentale → `peneplain` ou
`noise`, sédimentaire → `barrier`, corallienne → `atoll`), et il reste forçable. Le relief
de la terre, `h_land`, est d'abord calculé sur toute la grille, puis combiné avec la mer.

| `mode` | `h_land` |
|--------|----------|
| `flat` | `base_elevation_m` |
| `peneplain` | `base_elevation_m + fbm(q) · noise.amplitude_m` (amplitude tirée faible à l'étape 1) |
| `noise` | même formule, amplitude normale |
| `barrier` | `base + crest_height_m · smoothstep(0, W, s) + bruit faible`, avec `W` la plus grande distance à la côte : crête au milieu de la largeur |
| `atoll` | `ring_height_m + ring_noise_m · fbm(q)` (1 à 4 m, ou 30 à 60 m pour un makatea) |
| `volcanic_cone` | `max(base, max_v cône_v(r))`, plus les coulées (voir ci-dessous) |

**Pas de terrain plat.** Une zone plate s'écoule en lignes droites parallèles : sans pente,
le drainage suit la pente ε du remplissage des dépressions depuis le déversoir (diagnostic D5
de l'étape 3), et l'érosion en fait des canyons rectilignes. Tous les modes reçoivent donc,
en plus :

```
dome  = regional_dome_m · smoothstep(0, D, s_lissée)   // D : plus grande distance à la côte ; pas pour atoll et barrier
rough = roughness_m · fbm(q)                           // roughness_octaves (4 à 6) octaves, roughness_frequency (4 à 8) cycles par R
h_land += dome + rough
h_land  = smoothMax(h_land, 0,5 + 0,3 · (dome + roughness_m · (fbm + 1)), 1 m)
```

- **Le dôme régional** (5 à 30 m selon l'origine) donne à toute l'île une pente vers la mer.
  Il monte jusqu'au point le plus éloigné de la côte : avec `coast_dist / R`, comme dans la
  formule de départ, une île allongée n'en recevrait qu'une petite partie.
- **La rugosité** (2 à 10 m ; 0,5 à 1 m pour les atolls et barrières, plats par nature) crée
  des bassins versants dès le départ.
- **Le plancher** : une terre que le bruit fait passer sous la mer était ramenée à 0 m, en
  aplats. Elle repose maintenant sur un plancher bas qui garde la pente du dôme et une part
  de la rugosité.

**Volcans** (`initial_topography.volcano.*`, flux `"volcanoes"`) :

- **Placement :** par rejet, dans la moitié intérieure de l'île (distance à la côte au moins
  la moitié de son maximum), à au moins `min_spacing_m` des autres volcans (1000 essais par
  volcan ; il y a moins de volcans si l'île manque de place).
- **Mise à l'échelle :** chaque volcan est réduit en entier (rayon, hauteur, cratère, caldeira)
  pour que son pied arrive à la côte la plus proche de sa cheminée :
  `échelle = min(1, (distance à la côte + coast_falloff_m / 2) / radius_m)`, puis
  `h = échelle · cône(r / échelle)`. Sans ça, un grand cône près du bord était coupé par la
  côte, en demi-cône avec une falaise.
- **Profil :** `height_m · max(0, 1 − r / radius_m)^profile_exponent`. Le rayon vaut 0,6 à
  1 fois `R` (le cône remplit l'île), et la hauteur 0,15 à 0,45 fois le rayon (flancs
  plausibles).
- **Cratère :** `− crater_depth_m · max(0, 1 − r / crater_radius_m)²`.
- **Jonction avec le plateau :** cônes et plateau de base se rejoignent par un `smoothMax`
  sur la hauteur du plateau, sans arête circulaire.
- **Flancs irréguliers :** `+ flank_noise_m · fbm(q · flank_noise_frequency)` sur toute l'île
  (4 à 10 cycles par rayon, 2 à 3 octaves, flux `"flanks"`). Sur un cône parfait, la plus
  grande pente est radiale partout, et les coulées seraient des rayons droits. Ce relief les
  fait serpenter, et donnera prise à l'érosion. L'amplitude est proportionnelle à la longueur
  d'onde (`R / fréquence × [0,03 ; 0,08]`), ce qui borne les pentes du bruit. Une amplitude
  fixe à une longueur d'onde fine faisait des aiguilles.
- **Caldeira** (si `caldera`, à la place du cratère) :
  - le sommet est écrêté à l'altitude du profil au rayon de la caldeira ;
  - le fond est plat, `caldera_depth_m` plus bas ;
  - la paroi est en smoothstep, de largeur `caldera_rim_width_m`.
- **Coulées :** `lava_flow_count` par volcan. Chacune est une marche aléatoire qui part juste
  à l'extérieur du cratère ou de la caldeira, à un angle aléatoire. À chaque pas (une
  cellule) :
  - elle suit la plus grande pente de `h_land`, déviée aléatoirement de ± `lava_flow_turn_deg` ;
  - elle dépose une bosse gaussienne de hauteur `lava_flow_height_m` et de largeur
    `lava_flow_width_m`. Les bosses se combinent par `max` : deux coulées qui se croisent ne
    s'empilent pas ;
  - elle s'arrête en mer, ou sur une pente inférieure à `lava_flow_min_slope_deg`.

**Fond de lagon** (mode `atoll`, flux `"lagoon"` et `"pinnacles"`) :
`−lagoon_depth_m + lagoon_noise_m · fbm(q)`. Quelques pinacles gaussiens (`pinnacle_count`,
`pinnacle_radius_m`), placés au hasard dans le lagon, remontent jusqu'à
`−pinnacle_top_depth_m` (1 à 2 m sous la surface) sans jamais émerger. L'extérieur de l'atoll
garde le platier étroit et le tombant raide tirés par l'étape 1.

Combinaison :

```
r = smoothstep(0, coast_falloff_m / 2, |s|)
h = sea + (s ≥ 0 ? max(h_land, 0) · r : h_sea · r)
h += U(0, 1e-3)      // micro-perturbation, OBLIGATOIRE
```

`|s|` est ici la distance **lissée** à la côte. Les octaves fines du domain warp, qui
découpent la côte, rendent `coast_dist` irrégulière loin dans les terres et au large. Or les
altitudes suivent cette distance sur de larges bandes (rampe côtière, plateau et talus, crête
des barrières), et l'irrégularité devenait du relief : jusqu'à ±100 m sur une île volcanique
haute. Ces calculs utilisent donc `coast_dist` floutée (box blur deux fois, sur un quart de
`coast_falloff_m`). Un flou conserve une fonction linéaire : la distance reste juste loin de
la côte et quasi nulle sur la côte. Le côté terre / mer et le trait de côte restent ceux de
la distance brute.

Chaque côté rejoint le niveau de la mer **exactement sur la côte** (`s = 0`, `mask = 0,5`). Un
simple `lerp(h_sea, h_land, mask)` placerait la côte là où le mélange passe par 0. Quand
`h_land` est grand devant `|h_sea|` (183 m contre −13 m, par exemple), c'est loin dans la bande
de transition, là où les petites octaves du warp transforment la côte en confettis. Une
terre qui tomberait sous le niveau de la mer est ramenée au niveau de la mer.

La micro-perturbation (1 mm au plus) ne sert qu'à lever les égalités de hauteur pour le
drainage de l'étape 3 (sur le plateau continental, par exemple). Elle ne suffit pas à faire
s'écouler une zone plate naturellement : c'est le rôle du dôme et de la rugosité.

### 3.4 Conditions aux limites

L'anneau d'une cellule au bord reçoit `flags |= kFixed` et `h = sea + boundary_depth_m`.
L'étape 3 maintiendra ces cellules fixes : c'est là que l'eau et les sédiments quittent le
domaine.

### 3.5 Motifs de soulèvement (`tectonics.phases`)

On ne stocke que le **motif spatial** de chaque phase. Le taux (`rate_m_per_year`, toujours
positif) et la fenêtre de temps seront appliqués à l'étape 3 :
`cum_uplift += pattern_k × rate_k × dt` pendant la phase. Le **signe est porté par le
motif** : une subsidence est un dôme négatif.

Les phases dépendent de l'origine (étape 1) :

| Origine | Phases |
|---------|--------|
| volcanique | aucune ou un dôme faible |
| sédimentaire, corallienne | aucune ou une subsidence faible, plafonnée : son enfoncement total ne dépasse pas 50 % de l'altitude moyenne initiale estimée de l'île |
| continentale | 1 à 4 : `collision` 40 %, `dome` 30 %, `tilt`, `block_uplift`, `horst_graben` 10 % chacun, jamais de `subsidence` (sauf forcée) ; la première `collision` suit l'axe principal de l'île |

**Contrôle après tirage** (`meanPhaseUplift` dans `IslandParams.cpp`). On calcule le
soulèvement total moyen (`taux × durée × motif`) de chaque phase sur l'ellipse de l'île
(demi-longueur `R` le long de l'axe, `R / elongation` en travers). Tant que la somme est
négative (au-delà de la subsidence permise pour les îles non continentales), la phase qui
l'abaisse le plus est retirée. Une phase dont le type est forcé par la config n'est jamais
retirée. Le retrait est noté dans le journal de l'étape `parameters`, et la config résolue
(`toJson`) reste exacte : les phases suivantes sont renumérotées. Sur 200 seeds, cela arrive
à 3 îles continentales, à chaque fois à cause d'un `tilt`.

Le motif est calculé par [UpliftPattern.{hpp,cpp}](../src/wg/island/UpliftPattern.hpp),
partagé avec la géologie et le contrôle ci-dessus. Pour une phase de centre `c` et d'angle
`angle_deg`, `u` est la coordonnée le long de l'axe et `v` la coordonnée perpendiculaire,
**prises au point déformé** :

```
p'  = p + R · uplift_warp_amplitude · (fbm_a(q), fbm_b(q))     // basse fréquence : uplift_warp_frequency
(u, v) = repère de la phase en p'
mod = lerp(1 − uplift_modulation, 1, bruit(q))                 // uplift_modulation_frequency

gauss(t) = exp(−t²/2)                   // width_m joue le rôle d'un écart-type
taper(t) = 1 − smoothstep((1 − falloff)/2, 1/2, |t|)    // 1 au milieu, 0 aux extrémités

dome          P = gauss(|(u, v)| / width_m)
subsidence    P = −gauss(|(u, v)| / width_m)
collision     P = gauss(v / width_m) · taper(u / length_m)
block_uplift  P = smoothstep(0, falloff · width_m / 2, −sdf(u, v))  // rectangle à coins arrondis
horst_graben  P = +1 (horst) ou −negative_factor (graben) selon la parité de floor(v / width_m)
                  · smoothstep(0, falloff · width_m / 2, distance à la limite de bande)
                  · taper(u / length_m)                            // longueur finie
tilt          P = clamp(v / width_m, −1, 1), partie négative × negative_factor

P *= mod ;  P *= smoothstep(0, 1, mask) ;  P = 0 sur l'anneau FIXED
```

- **Le warp** (`uplift_warp_amplitude`, 0,1 à 0,25 R) : aucun bord ni axe n'est rectiligne.
- **La modulation** (`uplift_modulation`, 0,3 à 0,6) : le soulèvement n'est jamais uniforme.
- **Le bloc** est un rectangle aux coins arrondis sur la moitié de sa plus petite
  demi-taille (`sdf` est la distance signée à ce contour, négative dedans).
- **`negative_factor`** (0,3 par défaut, forçable) : un basculement ou des grabens ne
  doivent pas faire couler l'île.
- **Le `falloff`** est tiré dans [0,2 ; 0,35] pour `block_uplift` et `horst_graben` (des bords
  d'au moins 10 % de la largeur), et dans [0,2 ; 1] pour les autres.
- **La taille** : la longueur et la largeur d'une phase sont limitées à 1,2 fois le diamètre
  de l'île (de l'ellipse ci-dessus) dans leur direction.

**Failles dérivées.** L'étape 1 génère des failles sur les bords nominaux (non déformés)
de chaque `block_uplift` et `horst_graben` (`phaseFaults` dans `IslandParams.cpp`) :

- **bloc :** quatre failles, une par bord, côté soulevé vers l'intérieur, décalage
  `rate × durée` ;
- **horst / graben :** une faille par limite de bandes qui croise l'île, côté horst soulevé,
  décalage `(1 + negative_factor) × rate × durée` ;
- **pendage :** `fault_dip_deg` de la phase.

Ces traces droites ne sont qu'une approximation, listée pour information : la géologie
déplace les blocs entre des murs qui suivent le contour **déformé** du motif (voir 3.6).

**Convention d'une faille :** une trace centrée sur (`x`, `y`), le long de `angle_deg`. Son
côté **positif**, à gauche de la direction (normale `(−sin, cos)`), monte de `offset_m`.

Ces failles dérivées sont visibles dans la vue `parameters` (`geology.derived_faults`). Elles
sont toujours recalculées à partir des phases : la config ne peut pas les fixer, ce qui
préserve l'aller-retour de `toJson()`. Le calque `fault_traces` dessine les failles anciennes
(segments) et les murs réels des phases à failles (là où le bloc change d'une cellule à
l'autre au niveau de la mer).

### 3.6 Géologie ([Geology.{hpp,cpp}](../src/wg/island/Geology.hpp))

`Geology` décrit la **roche en place** et répond à « quel matériau en (x, y, z) ? ». Elle se
construit à partir des paramètres (la profondeur cumulée du bas de chaque strate, la
géométrie des failles...), en quelques microsecondes.

```
material(x, y, z, cum_uplift, z_ref, D_fold):
    z_rock = z − cum_uplift − D_fold(x, y) − D_fault(x, y, z)   // les strates bougent avec la roche
    intrusions (testées d'abord) : un dôme de rayon r, de sommet top_m
        dist2d < r  et  z_rock < top_m − dome_k · (dist2d / r)² · r   → matériau de l'intrusion
    depth = z_ref − z_rock
    depth < 0          → strata[0]   (au-dessus de la pile : sa couche du haut)
    i = upper_bound(bottoms, depth)  // recherche binaire
    i < nombre de strates → strata[i], sinon basement_material
```

- **Pli** (`geology.folding`, précalculé dans le calque `fold_offset`). Avec `u` le long de
  l'axe du pli (l'axe principal de l'île pour une île continentale) et `v` en travers :
  `φ = 2π · v / wavelength_m`, puis
  `D_fold = amplitude_m · sin(φ + asymmetry · sin(φ)) + tan(plunge_deg) · u`. L'asymétrie
  déverse les plis, et le plongement incline leur axe. La longueur d'onde vaut 0,4 à 1,5 fois
  `R`, et l'amplitude 3 à 20 % de la longueur d'onde. Tirées indépendamment, elles donnaient
  des plis aux flancs presque verticaux, qui se répétaient tous les 300 m.
- **Failles anciennes** (`geology.faults`, tirées ; dépendent de `z`) :
  - **plan :** il contient la trace au niveau de la mer et plonge de `dip_deg` vers le côté
    abaissé. À `d` mètres sous la mer, il est décalé de `d / tan(dip)` de ce côté ;
  - **décalage :** un point du côté positif du plan (le côté soulevé, à gauche de la
    trace), dont la projection tombe dans la longueur de la faille, reçoit `+offset_m` ;
  - **extrémités :** le décalage s'atténue sur une fraction `geology.fault_tip_taper` de la
    demi-longueur.

  Ces failles sont indépendantes et leurs décalages s'additionnent ; elles sont décalées dès
  le départ.
- **Phases à failles** (`block_uplift`, `horst_graben`, `UpliftPattern::blockShare`) : elles
  déplacent des **blocs entiers** entre des murs inclinés de `fault_dip_deg`. Au niveau de la
  mer, les murs passent au milieu des bords adoucis du motif déformé, et en profondeur ils
  s'écartent du bloc soulevé, comme les failles normales d'un horst :
  - **bloc :** il monte de `U · mod` (`U` le soulèvement total de la phase, `mod` sa
    modulation) ;
  - **bandes :** les horsts montent de `U · mod`, les grabens descendent de
    `negative_factor · U · mod`.

  Le déplacement est pondéré par l'**avancement de la phase** (`setPhaseProgress`). Il est nul
  à l'étape 2, puisque la phase n'a pas encore agi : un décalage des strates sans relief
  correspondant n'aurait pas de sens. Les failles dérivées (`geology.derived_faults`) ne
  s'additionnent pas : ce ne sont que les traces nominales de ces murs, et quatre failles
  autour d'un bloc le soulèveraient quatre fois.
- **Intrusions** (`geology.intrusions`) : des dômes d'un seul matériau. Pour une île
  continentale qui a une phase `collision`, elles sont placées dans la zone de la chaîne.
- **`z_ref`** vaut `sea + geology.strata_top_m`. Pour une origine volcanique, c'est
  `h_initial` : les couches de lave et de tuf sont parallèles aux flancs.
- **Les ids de matériaux** indexent `materialNames()`, les noms de `geology.materials` triés.
  Ce sont les catégories du calque `surface_material`, rempli avec la consultation complète en
  `z = h`.

**Pour l'étape 3.** Elle fera avancer `setPhaseProgress` avec le temps. Les strates des
phases à failles bougeront alors avec leurs blocs : la part de ces phases dans `cum_uplift`
ne devra donc pas aussi décaler les strates (`cum_uplift` sert au relief, les blocs aux
roches), sinon elles monteraient deux fois.

### 3.7 Vérifications et suivi en direct

- **Altitudes finies :** en fin d'étape, toute altitude non finie lève une `WorldError`.
- **Progression :** elle est rapportée toutes les 32 lignes (masque : 0 → 40 %, altitudes :
  40 → 75 %, motifs : 75 → 90 %). Le viewer montre donc les calques se remplir.

## 4. Ce que vérifier dans le viewer

| Calque | Attendu |
|--------|---------|
| `h` | île, liseré bleu clair (plateau), pente ombrée vers un fond plus sombre, cadre uniforme au bord |
| `mask` | blanc sur l'île, noir en mer, transition douce |
| `coast_dist` | côte blanche, terre rouge vers l'intérieur, mer bleue vers le large |
| `uplift_pattern_k` | dôme ou chaîne orientée aux contours irréguliers (jamais de bord rectiligne), modulé, s'atténuant aux extrémités, nul en mer |
| `surface_material` | couleurs de la légende ; changer `geology.strata_top_m` change ce qui affleure |

Les tests ([tests/main.cpp](../tests/main.cpp), `testIslandInit`, `testIslandFitsInMap`)
vérifient :
- **reproductibilité :** déterminisme au bit près ;
- **validité :** absence de NaN, anneau `FIXED` à la bonne profondeur, motifs dans [−1, 1] ;
- **contour :** sur 20 seeds, mer sur les bords et raccord sans marche au fond imposé.

## 5. Limites connues et suite

- **Îlots et lacs.** Les lobes séparés donnent plusieurs îles. Quand une baie tombe dans un
  autre lobe, elle crée un lagon fermé. `shape.keep_satellite_islets` et `min_islet_area_m2`
  ne sont pas encore appliqués : ce sera fait plus tard (hydrologie, nettoyage).
- **Outils de validation :**
  - **coupe verticale du viewer** (`X`) : on y voit les plis, les failles décalées en
    profondeur, les dômes d'intrusion et les couches volcaniques parallèles aux flancs ;
  - **galerie** (`wgen --gallery … --origin all`) : pour comparer la variété des îles par
    origine.
