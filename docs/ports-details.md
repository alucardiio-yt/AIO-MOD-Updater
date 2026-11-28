# Información opcional de Ports para AIO MOD 6.0

El catálogo existente `ports.json` conserva sus enlaces e iconos. No hay que cambiarlo para usuarios de versiones anteriores.

Cuando estés listo para publicar las fichas, usa `docs/ports-details.example.json` como plantilla y crea `ports-details.json` en la raíz del repositorio `archives-nx`. Rellena solamente los juegos que necesiten información adicional y sustituye los campos vacíos antes de publicar. La clave debe coincidir exactamente con el nombre del juego en `ports.json`, quitando el sufijo ` (PARTE 1)`, ` (PARTE 2)`, etc. en los juegos divididos.

- `author`: texto que aparecerá como autor.
- `description`: indicaciones breves del juego; conviene que no superen unas 350 letras para que quepan en la pantalla. Puede ser un texto único o un objeto con claves `es`, `en-US` y `fr` para los idiomas que ofrece el menú de AIO MOD. Si falta una traducción, la app busca `es` y luego `en-US`.
- `recommended_space_mb` es opcional y normalmente no necesitas escribirlo. Si falta, AIO MOD estima el espacio recomendado con el tamaño de descarga, cuando el servidor lo informa.

En las versiones con el aviso «Actualización», escribe `Actualización 0.5.0` en la descripción del juego. También puedes usar `(Actualización v0.5.0 27-11-26)`. Basta con incluir la marca en español. Cuando cambie la versión, sustituye la marca anterior. La lista muestra el aviso hasta que el usuario abre la descripción. Para comprobar una edición en GitHub, cierra AIO MOD y vuelve a abrirla.

No copies los enlaces ni los iconos en este archivo: siguen en `ports.json`. Las partes tampoco se duplican. AIO MOD las agrupa por los sufijos ` (PARTE 1)`, ` (PARTE 2)`, etc., siempre que estén completas y sean ZIP. Al instalar, descarga y extrae una parte por vez. Si falta una parte, las entradas quedan separadas como en el catálogo original. Si este archivo opcional no está disponible, los juegos siguen apareciendo y se pueden instalar. Los cambios publicados en `ports-details.json` se verán al volver a abrir AIO MOD.
