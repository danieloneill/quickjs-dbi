# quickjs-dbi
Basic libdbi implementation for [QuickJS](https://bellard.org/quickjs/).

**This code currently builds against QuickJS-2025-09-13**

## Building
Edit Makefile and point QJSPATH to your quickjs root then just build with *make*.

Now you should be able to run the examples:
 * example.js

## Documentation

DBI:
 * open(type, opts) -> DBIHandle
   * type - string - a valid dbd driver name
   * opts - jsobject - dictionary containing connection parameters as keys and values

DBIHandle:
 * exec(querystr, params) -> boolean
   * querystr - string - the query string
   * params - jsobject/array - parameters to fill placeholders within a query:
    - if `params` is an array, query placeholders "?" will be replaced, in turn, with the values
    - if `params` is a dictionary object, query placeholders ":&lt;key&gt;" will be replaced with matching key values in the object
 * query(querystr, params) -> DBIResult
   * same parameters as `exec`, but returns a `DBIResult`
 * close()

DBIResult:
 * next() -> boolean
   * first call will attempt to place the cursor at the first result row. subsequent calls will advance to the next row.
   * if there are no (more) rows, returns FALSE, otherwise TRUE.
 * get(field) -> variant
   * `field` may be either the name of the field, or a numeric index within the results.
   * return value depends on the field type in the query result
 * numfields() -> number
 * numrows() -> number
 * toArray(asdict) -> Array
   * if `asdict` is true, result rows will contain jsobject dictionaries associating each column name to each value.
   * if `asdict` is false, result rows will contain Arrays of the values.

See the example.

I'm aware that things like setting the current row, prepared statements, and other basic methods aren't implemented. Please don't hesitate to offer a PR if you get around to implementing such things before me.

This module hasn't been tested very thoroughly, bug reports are welcome.

Also check out my [quickjs-net](https://github.com/danieloneill/quickjs-net), [quickjs-hash](https://github.com/danieloneill/quickjs-hash) and [quickjs-wolfssl](https://github.com/danieloneill/quickjs-wolfssl) modules.

