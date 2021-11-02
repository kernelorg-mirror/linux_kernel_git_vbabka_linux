// SLAB/SLUB: mass conversion of inline functions exported by both
//
// convert nearest_obj(), obj_to_index() and objs_per_slab_page()
//
// usage:
// spatch --include-headers --no-includes slab_common.cocci include/linux/slab_def.h include/linux/slub_def.h mm/slab.h mm/kasan/*.c mm/kfence/kfence_test.c mm/memcontrol.c mm/slab.c mm/slub.c

@ rename_objs_per_slab_page @
@@
-objs_per_slab_page(
+objs_per_slab(
 ...
 )
{
...
}

@ rename_objs_per_slab_page_callers @
@@
-objs_per_slab_page(
+objs_per_slab(
 ...
 )

// for all functions (with exceptions), change any "struct page *page"
// parameter to "struct slab *slab" in the signature, and generally all
// occurences of "page" to "slab" in the body - with some special cases.
@ convert_param_const_struct_page_ptr @
identifier fn =~ "obj_to_index|objs_per_slab";
expression E;
@@

 fn(...,
-   const struct page *page
+   const struct slab *slab
    ,...)

{
<...
(
- page_address(page)
+ slab_address(slab)
|
- page
+ slab
)
...>
}

// for all functions (with exceptions), change any "struct page *page"
// parameter to "struct slab *slab" in the signature, and generally all
// occurences of "page" to "slab" in the body - with some special cases.
@ convert_param_struct_page_ptr @
identifier fn =~ "nearest_obj";
expression E;
@@

 fn(...,
-   struct page *page
+   const struct slab *slab
    ,...)

{
<...
(
- page_address(page)
+ slab_address(slab)
|
- page
+ slab
)
...>
}

// convert params of call sites
@ convert_caller_params @
identifier fn =~ "nearest_obj|obj_to_index|objs_per_slab";
expression E;
@@
fn(...,
(
- slab_page(E)
+ E
|
- virt_to_page(E)
+ virt_to_slab(E)
|
- virt_to_head_page(E)
+ virt_to_slab(E)
|
- page
+ page_slab(page)
)
  ,...)



