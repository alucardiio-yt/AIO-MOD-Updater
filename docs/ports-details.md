# Información opcional de Ports para AIO MOD 6.0

El catálogo existente `ports.json` conserva sus enlaces e iconos. No hay que cambiarlo para usuarios de versiones anteriores.

Cuando estés listo para publicar las fichas, copia `ports-details.example.json` a `ports-details.json` en la raíz de `archives-nx` y rellena solamente los juegos que necesiten información adicional. La clave debe coincidir exactamente con el nombre del juego en `ports.json`, quitando el sufijo ` (PARTE 1)`, ` (PARTE 2)`, etc. en los juegos divididos.

- `author`: texto que aparecerá como autor.
- `description`: indicaciones breves del juego; conviene que no superen unas 350 letras para que quepan en la pantalla. Puede ser un texto único o un objeto con claves `es`, `en-US`, etc. para mostrar cada idioma.
- `recommended_space_mb`: espacio recomendado en MB. Si vale `0` o falta, AIO MOD calcula una estimación con el tamaño de descarga, cuando el servidor lo informa.

Las partes no se duplican en este archivo. AIO MOD las agrupa por los sufijos ` (PARTE 1)`, ` (PARTE 2)`, etc., siempre que estén completas y sean ZIP. Al instalar, descarga y extrae una parte por vez. Si falta una parte, las entradas quedan separadas como en el catálogo original. Si este archivo opcional no está disponible, los juegos siguen apareciendo y se pueden instalar.
