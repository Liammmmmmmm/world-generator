# Étape 1 : paramètres

Transformer le fichier de config, où presque tout vaut `null`, en un jeu **complet**
de paramètres typés, tirés de la seed et reproductibles.

← [Vue d'ensemble](README.md) · [Étape 2](etape-2-initialisation.md) →

---

## 1. Le principe

La convention est simple : **`null` = tiré de la seed, toute autre valeur override le tirage.**

```jsonc
"shape": {
  "radius_ratio": null,   // tiré dans [0.4, 0.8]
  "lobe_count": 3,        // forcé à 3
  "noise": { "lacunarity": 2.0 }   // constante (pas de tirage pour ce paramètre)
}
```

Le résultat est une struct C++ entièrement remplie (`IslandParams`), plus l'historique de
chaque valeur et de sa provenance (`ParamEntry`) : seed, config ou défaut fixe.

## 2. Où ça se passe

| Fichier | Rôle |
|---------|------|
| [core/Config.cpp](../src/wg/core/Config.cpp) | `WorldConfig::load` : parse le JSONC (commentaires autorisés), lit `seed` (entier, ou texte hashé), appelle `resolveIslandParams` |
| [params/ParamResolver.{hpp,cpp}](../src/wg/params/ParamResolver.hpp) | la **mécanique** : un tirage par paramètre, overrides, listes, validation, enregistrement |
| [params/IslandParams.hpp](../src/wg/params/IslandParams.hpp) | la **struct typée** de tous les paramètres + `ResolvedParams` |
| [params/IslandParams.cpp](../src/wg/params/IslandParams.cpp) | les **distributions** : une ligne par paramètre, regroupées par section |
| [gen/stages/ParametersStage.cpp](../src/wg/gen/stages/ParametersStage.cpp) | l'étape du pipeline, qui publie les paramètres dans le `WorldState` |

**Pourquoi la résolution a lieu au chargement, et pas dans l'étape.** Deux raisons :
- les erreurs (faute de frappe, mauvais type) apparaissent avant que quoi que ce soit ne
  tourne ;
- les dimensions du monde (`world.*`) sont nécessaires pour créer le `WorldState` initial.

L'étape `parameters` se contente ensuite de faire `world.setParams(config.params)`. Les
paramètres font ainsi partie des snapshots : le viewer les affiche, et `.wgw` les embarque.

## 3. L'algorithme de résolution

### 3.1 Un flux aléatoire par paramètre

```cpp
double ParamResolver::unit(path) {               // valeur uniforme dans [0, 1)
    Rng rng(deriveSeed(seed, "param:" + path));  // ex. "param:geology.strata[2].thickness_m"
    return rng.nextDouble();
}
```

Chaque paramètre a **son propre** flux, dérivé du chemin. On n'utilise donc jamais un RNG
partagé qui avancerait d'un tirage à l'autre. Conséquences :

- **une override ne décale rien :** forcer `shape.lobe_count` ne change ni
  `shape.lobe_spread` ni aucun autre tirage ;
- **ajouter un paramètre dans le code ne change aucune valeur existante ;**
- **l'ordre de résolution n'a aucun effet** sur les valeurs. On peut le réorganiser selon
  les dépendances.

### 3.2 Toujours tirer, puis overrider

Toutes les fonctions aléatoires suivent le même schéma
([ParamResolver.cpp](../src/wg/params/ParamResolver.cpp), `sample`) :

```cpp
const double drawn = f(unit(path));      // 1. tirage, TOUJOURS fait
if (config contient path et non null)    // 2. override éventuelle (type vérifié)
    return valeur_config;                //    enregistrée comme ParamSource::Config
return drawn;                            //    sinon ParamSource::Seed
```

Les fonctions disponibles :

| Fonction | Distribution |
|----------|--------------|
| `uniform(path, a, b)` | uniforme dans [a, b] |
| `logUniform(path, a, b)` | log-uniforme : pour les grandeurs sur plusieurs ordres (épaisseurs de strates, longueurs d'onde) |
| `integer(path, a, b)` | entier uniforme, bornes incluses |
| `chance(path, p)` | booléen, vrai avec probabilité `p` |
| `pick(path, poids, autorisés)` | choix pondéré ; l'override peut prendre toute valeur de `autorisés` |
| `sample(path, f)` / `sampleInteger` | loi quelconque : `f(u)` transforme l'uniforme `u` |
| `point(path, …)` | point [x, y] dans un rectangle (2 tirages du même flux) |

Les **paramètres sans aléatoire** (`world.width`, `simulation.resolution`…) passent par
`number`, `whole`, `flag`, `object` : la valeur de la config, ou un défaut fixe
(`ParamSource::Default`).

### 3.3 Dépendances entre paramètres

Une distribution peut dépendre de paramètres **déjà résolus**, et elle utilise alors leur
valeur **finale** (override comprise) :

```cpp
// la longueur d'onde des plis est relative au rayon de l'île
f.wavelengthM = r.sample("geology.folding.wavelength_m",
    [&](double u) { return radius * logLerp(0.4, 1.5, u); });
```

Le nombre de tirages ne change pas (toujours un `u` par paramètre) ; seule la transformation
de `u` dépend du parent. Forcer `origin.type: "volcanic"` change donc les matériaux tirés
pour les strates, sans perturber aucun flux.

Les principales dépendances :

| Paramètre | Dépend de |
|-----------|-----------|
| `initial_topography.volcano.caldera` | `origin.age` (vieux = caldeira plus probable) |
| `initial_topography.mode`, `base_elevation_m` | `origin.type` |
| `geology.strata[i].material`, `basement_material`, `strata_top_m`, `folding.enabled` | `origin.type` |
| tailles du relief (volcans, plis...) | rayon de l'île `R` |
| `shape.lobe_radius_max` | `lobe_radius_min` |
| `shape.*` (lobes, dispersion, élongation, warp, baies, plateau, pente du talus) | `origin.type` : chaque origine a sa silhouette (voir le tableau dans `island_params.jsonc`) |
| `geology.folding.axis_angle_deg` (île continentale) | `shape.main_axis_deg` : un axe principal partagé |
| `shape.shelf_width_m` | place libre entre l'île et le bord de carte (voir 3.5) |
| `simulation.boundary_depth_m`, `shape.talus_width_m` | place libre, plateau, `shape.talus_slope_deg` |
| `initial_topography.volcano.crater_depth_m`, `min_spacing_m` | `height_m`, `radius_m` |

L'ordre d'appel dans `resolveIslandParams` suit ces dépendances : world, origin, shape,
simulation, bathymétrie, topographie, géologie, climat.

### 3.4 Listes

Les listes (aujourd'hui `geology.strata`) passent par `listSize(liste, compteur, min, max)` :

| Dans la config | Nombre d'éléments |
|----------------|-------------------|
| la liste est un tableau | sa longueur. Si `*_count` est aussi donné et différent, c'est une erreur |
| liste `null`, `*_count` donné | `*_count` |
| les deux `null` | `*_count` tiré dans [min, max] |

Chaque champ d'élément est ensuite résolu comme un paramètre normal, avec un chemin indexé
(`geology.strata[2].thickness_m`). Un élément de la config peut donc ne fixer que certains
champs : `{ "material": "granite" }` fixe le matériau et tire l'épaisseur.

### 3.5 Bathymétrie qui tient dans la carte

La carte est finie : entre l'île et le bord, il faut caser le plateau puis la pente jusqu'au
fond imposé au bord. Les défauts sont donc calculés à partir de la place disponible :

```
place libre  A = demi-taille − R · (1 + lobe_blend/4 + shape.noise.amplitude / elongation)   // au-delà de la côte la plus lointaine
shelf_width  = clamp(A · part, 10, 1500)       part : volc. 3-10 %, cont. 10-35 %, séd. 30-60 % (cor. : 10 à 50 m)
talus_slope  = volc. [15, 30]°, cont. [8, 20]°, séd. [3, 10]°, cor. [35, 50]°
boundary     = max(U[-1500, -200],  shelf_depth − (A − shelf_width) · tan(talus_slope))
talus_width  = (shelf_depth − boundary) / tan(talus_slope)
```

Sur une carte de 4 km, ça donne typiquement un fond entre −150 et −400 m avec une pente
plausible, au lieu d'un mur à −1400 m. Si tu forces une valeur (par exemple
`boundary_depth_m: -1500`), elle est respectée, et c'est la largeur du talus qui s'adapte.

### 3.6 Validation

- **Types :** un nombre attendu, un entier, un booléen, une chaîne dans une liste de choix...
  L'erreur cite le chemin, par exemple `'shape.lobe_count' must be an integer or null`.
- **Références :** les matériaux des strates et du socle doivent exister dans
  `geology.materials`. On peut y ajouter ses propres matériaux.
- **Cohérence :** `world.min_y < max_y`, `lobe_count ≥ 1`, `lobe_radius_min ≤ max`...
- **Clés inconnues.** Chaque lecture marque son chemin comme « consommé ». À la fin,
  `checkNoUnknownKeys` parcourt la config, et toute feuille non consommée est une erreur :
  c'est ce qui attrape les fautes de frappe.

## 4. Les structures produites

```cpp
struct ResolvedParams {
    IslandParams values;            // tout, typé : p.shape.radiusRatio, p.geology.strata[i]...
    std::vector<ParamEntry> entries; // { path, value (json), source } dans l'ordre d'affichage
    nlohmann::json config;          // la config d'entrée, telle quelle
    ordered_json toJson() const;    // la config complète, toutes valeurs remplies
};
```

Quelques points d'usage :
- **Accès depuis une étape :** `const IslandParams& p = world.params().values;`.
- **Rayon de l'île :** `p.islandRadiusM()` renvoie `R = radius_ratio × demi-taille de la carte`.
- **Reproductibilité :** `toJson()`, réinjecté comme config, redonne exactement les mêmes
  paramètres (un test le vérifie). C'est ce qu'écrit `wgen --params`.
- **Format `.wgw` :** il stocke la config d'entrée. Au rechargement, on re-résout, ce qui est
  possible parce que la résolution est déterministe.

## 5. Conventions d'unités

- **Unités physiques :** mètres et degrés.
- **Fréquences de bruit :** en **cycles par rayon d'île R**. Le résultat ne dépend donc pas de
  la résolution de la grille.
- **Coordonnées** (`x`, `y`, `center`) : en mètres, avec l'**origine au centre** de la carte.
- **Altitudes et profondeurs :** relatives au niveau de la mer (`world.sea_level_y`).

## 6. Ajouter un paramètre

1. Ajouter le champ dans la struct de [IslandParams.hpp](../src/wg/params/IslandParams.hpp).
2. Le résoudre dans la fonction de section de [IslandParams.cpp](../src/wg/params/IslandParams.cpp) :
   `s.monParam = r.uniform("section.mon_param", min, max);`. Placer l'appel après les
   paramètres dont il dépend.
3. L'ajouter dans `island_params.jsonc` (`null` ou valeur), avec un commentaire donnant sa
   plage.

Il n'y a rien d'autre à faire : l'affichage dans le viewer, l'export `--params` et la
détection des clés inconnues sont automatiques.

## 7. Dans le viewer

L'étape `1. parameters` affiche la liste complète, triée par section :
- **vert :** tiré de la seed ;
- **blanc :** venant du fichier ;
- **gris :** défaut fixe.

La vue `parameters` reste accessible depuis les étapes suivantes.
