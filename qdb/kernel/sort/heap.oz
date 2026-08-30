(block
  (pragma language overloads)

  ;; Item's operator < must define a strict weak order. The heap keeps the last
  ;; item in that order at index 0. A caller may overload < for an item type
  ;; which refers to columnar data.

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
  (fun heap_sift_down [Item]
       ((var items <ptr Item>)
        (var size i64)
        (var root i64))
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
                          (< (index items left) (index items right)))
                    (block
                      (= candidate right)))
                  (if (< item (index items candidate))
                    (block
                      (= items [current] (index items candidate))
                      (= current candidate))
                    (block
                      (= done #t))))
                (block
                  (= done #t)))))
          (= items [current] item)))))

  (fun heap_sift_up [Item]
       ((var items <ptr Item>)
        (var child i64))
    (block
      (var item = (index items child))
      (var current = child)
      (var done bool)
      (= done #f)
      (while (&& (> current 0) (! done))
        (block
          (var parent = (// (- current 1) 2))
          (if (< (index items parent) item)
            (block
              (= items [current] (index items parent))
              (= current parent))
            (block
              (= done #t)))))
      (= items [current] item)))

  (fun heapify [Item]
       ((var items <ptr Item>)
        (var size i64))
    (block
      (if (> size 1)
        (block
          (var root = (// (- size 2) 2))
          (var done bool)
          (= done #f)
          (while (! done)
            (block
              (call heap_sift_down items size root)
              (if (== root 0)
                (block
                  (= done #t))
                (block
                  (= root (- root 1))))))))))

  ;; The caller owns the storage and must reserve space for one more item.
  (fun heap_push [Item]
       ((var items <ptr Item>)
        (var size i64)
        (var item Item)) -> i64
    (block
      (= items [size] item)
      (call heap_sift_up items size)
      (return (+ size 1))))

  ;; Returns the resulting size, so a caller can tell that an empty heap kept
  ;; nothing instead of losing the item without a trace.
  (fun heap_replace_top [Item]
       ((var items <ptr Item>)
        (var size i64)
        (var item Item)) -> i64
    (block
      (if (<= size 0)
        (block
          (return 0)))
      (= items [0] item)
      (call heap_sift_down items size 0)
      (return size)))

  ;; The removed item is left at items[new_size].
  (fun heap_pop [Item]
       ((var items <ptr Item>)
        (var size i64)) -> i64
    (block
      (if (<= size 0)
        (block
          (return 0)))
      (var new_size = (- size 1))
      (if (> new_size 0)
        (block
          (call heap_swap items 0 new_size)
          (call heap_sift_down items new_size 0)))
      (return new_size)))

  (fun heap_sort [Item]
       ((var items <ptr Item>)
        (var size i64))
    (block
      (call heapify items size)
      (var heap_size = size)
      (while (> heap_size 1)
        (block
          (= heap_size (call heap_pop items heap_size))))))
)
