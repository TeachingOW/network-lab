# bulkapi route spec: read at BUILD time by tools/gen_routes.py,
# which emits build/routes_gen.h (the compiled-in router).
#
# Grammar (one route per line, '#' starts a comment):
#   METHOD  /path/with/:params  ->  handler_name  [maxbody=SIZE]
#
#   METHOD   GET | POST | PUT | DELETE | PATCH | HEAD
#   :name    a path parameter (one non-empty segment), exposed to the
#            handler as req.params.v[i] in order of appearance
#   handler  C++ function `void h_<handler_name>(const Req&, Res&)`
#            that you implement in src/handlers.cpp (missing => link error)
#   maxbody  largest accepted request body; suffix k/m/g. Default 1m.
#            Enforced from the Content-Length header, before buffering.
#
# Literal segments always win over :param segments, regardless of line order.

GET     /health               -> health
GET     /stats                -> stats

GET     /items/:id            -> get_item
PUT     /items/:id            -> put_item         maxbody=1m
DELETE  /items/:id            -> delete_item

POST    /items/bulk           -> bulk_insert      maxbody=64m
POST    /items/bulk/get       -> bulk_get         maxbody=16m
POST    /items/bulk/delete    -> bulk_delete      maxbody=16m
