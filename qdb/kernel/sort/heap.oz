(block
  (pragma language overloads)

  ;; The heap keeps the last item of the order at index 0.
  ;;
  ;; Every entry point takes a user context that the comparison receives. The
  ;; short forms pass 0, so an item type with its own operator < needs nothing
  ;; extra. A caller that keeps the sort keys outside the items overloads
  ;; heap_less on its own context type instead of growing the item.
  (fun heap_less [Ctx Item]
       ((var ctx Ctx)
        (var a Item)
        (var b Item)) -> bool
    (block
      (return (< a b))))

  (fun heap_swap [Item]
       ((var items <ptr Item>)
        (var left i64)
        (var right i64))
    (block
      (var item = (index items left))
      (= items [left] (index items right))
      (= items [right] item)))

  ;; Moves a hole down instead of swapping at every level: one store per level
  ;; plus one at the end, rather than three.
  (fun heap_sift_down [Ctx Item]
       ((var items <ptr Item>)
        (var size i64)
        (var root i64)
        (var ctx Ctx))
    (block
      (if (< root size)
        (block
          (var item = (index items root))
          (var current = root)
          (var done bool)
          (= done #f)
          (while (! done)
            (block
              (var left = (+ (* current 2) 1))
              (if (< left size)
                (block
                  (var candidate = left)
                  (var right = (+ left 1))
                  (if (&& (< right size)
                          (call heap_less ctx
                                (index items left) (index items right)))
                    (block
                      (= candidate right)))
                  (if (call heap_less ctx item (index items candidate))
                    (block
                      (= items [current] (index items candidate))
                      (= current candidate))
                    (block
                      (= done #t))))
                (block
                  (= done #t)))))
          (= items [current] item)))))

  (fun heap_sift_up [Ctx Item]
       ((var items <ptr Item>)
        (var child i64)
        (var ctx Ctx))
    (block
      (var item = (index items child))
      (var current = child)
      (var done bool)
      (= done #f)
      (while (&& (> current 0) (! done))
        (block
          (var parent = (// (- current 1) 2))
          (if (call heap_less ctx (index items parent) item)
            (block
              (= items [current] (index items parent))
              (= current parent))
            (block
              (= done #t)))))
      (= items [current] item)))

  (fun heapify [Ctx Item]
       ((var items <ptr Item>)
        (var size i64)
        (var ctx Ctx))
    (block
      (if (> size 1)
        (block
          (var root = (// (- size 2) 2))
          (var done bool)
          (= done #f)
          (while (! done)
            (block
              (call heap_sift_down items size root ctx)
              (if (== root 0)
                (block
                  (= done #t))
                (block
                  (= root (- root 1))))))))))

  ;; The caller owns the storage and must reserve space for one more item.
  (fun heap_push [Ctx Item]
       ((var items <ptr Item>)
        (var size i64)
        (var item Item)
        (var ctx Ctx)) -> i64
    (block
      (= items [size] item)
      (call heap_sift_up items size ctx)
      (return (+ size 1))))

  ;; Returns the resulting size, so a caller can tell that an empty heap kept
  ;; nothing instead of losing the item without a trace.
  (fun heap_replace_top [Ctx Item]
       ((var items <ptr Item>)
        (var size i64)
        (var item Item)
        (var ctx Ctx)) -> i64
    (block
      (if (<= size 0)
        (block
          (return 0)))
      (= items [0] item)
      (call heap_sift_down items size 0 ctx)
      (return size)))

  ;; The removed item is left at items[new_size].
  (fun heap_pop [Ctx Item]
       ((var items <ptr Item>)
        (var size i64)
        (var ctx Ctx)) -> i64
    (block
      (if (<= size 0)
        (block
          (return 0)))
      (var new_size = (- size 1))
      (if (> new_size 0)
        (block
          (call heap_swap items 0 new_size)
          (call heap_sift_down items new_size 0 ctx)))
      (return new_size)))

  (fun heap_sort [Ctx Item]
       ((var items <ptr Item>)
        (var size i64)
        (var ctx Ctx))
    (block
      (call heapify items size ctx)
      (var heap_size = size)
      (while (> heap_size 1)
        (block
          (= heap_size (call heap_pop items heap_size ctx))))))

  ;; Short forms for items that compare with their own operator <.
  (fun heap_sift_down [Item]
       ((var items <ptr Item>)
        (var size i64)
        (var root i64))
    (block
      (call heap_sift_down items size root 0)))

  (fun heap_sift_up [Item]
       ((var items <ptr Item>)
        (var child i64))
    (block
      (call heap_sift_up items child 0)))

  (fun heapify [Item]
       ((var items <ptr Item>)
        (var size i64))
    (block
      (call heapify items size 0)))

  (fun heap_push [Item]
       ((var items <ptr Item>)
        (var size i64)
        (var item Item)) -> i64
    (block
      (return (call heap_push items size item 0))))

  (fun heap_replace_top [Item]
       ((var items <ptr Item>)
        (var size i64)
        (var item Item)) -> i64
    (block
      (return (call heap_replace_top items size item 0))))

  (fun heap_pop [Item]
       ((var items <ptr Item>)
        (var size i64)) -> i64
    (block
      (return (call heap_pop items size 0))))

  (fun heap_sort [Item]
       ((var items <ptr Item>)
        (var size i64))
    (block
      (call heap_sort items size 0)))
)
