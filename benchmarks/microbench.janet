(import /build/jit :as jit)

(defn jit-perf-cmp [loops f & args]
  (def warmup-itterations 100)
  (def itterations loops)
  (def jit-f (jit/jitable f :error))

  (repeat warmup-itterations
    (jit-f ;args))

  (assert (jit/compiled? jit-f) "JIT fail to compile")

  (def interp-start (os/clock :monotonic))
  (repeat itterations
    (f ;args))
  (def interp-elapsed (- (os/clock :monotonic) interp-start))

  (def jit-start (os/clock :monotonic))
  (repeat itterations
    (jit-f ;args))
  (def jit-elapsed (- (os/clock :monotonic) jit-start))

  (prinf "| %.04f ips | %.04f ellapsed | " (/ itterations interp-elapsed) interp-elapsed)
  (printf "%.04f ips | %.04f ellapsed  | %.02fx faster |" (/ itterations jit-elapsed) jit-elapsed  (/ interp-elapsed jit-elapsed)))

(defn lerp
  [a b t]
  (+ a (* t (- b a))))

(defn fade [t]
  (- (* 6 (* t t t t t))
     (* 15 (* t t t t))
     (* -10 (* t t t))))

(defn dot
  [x1 y1 x2 y2]
  (+ (* x1 x2) (* y1 y2) ))

(defn my-compare [x y] (cmp x y))

(def perm-table
  [151 160 137  91  90  15 131  13 201  95  96  53 194 233   7 225
   140  36 103  30  69 142   8  99  37 240  21  10  23 190   6 148
   247 120 234  75   0  26 197  62  94 252 219 203 117  35  11  32
   57  177  33  88 237 149  56  87 174  20 125 136 171 168  68 175
   74  165  71 134 139  48  27 166  77 146 158 231  83 111 229 122
   60  211 133 230 220 105  92  41  55  46 245  40 244 102 143  54
   65   25  63 161   1 216  80  73 209  76 132 187 208  89  18 169
   200 196 135 130 116 188 159  86 164 100 109 198 173 186   3  64
   52  217 226 250 124 123   5 202  38 147 118 126 255  82  85 212
   207 206  59 227  47  16  58  17 182 189  28  42 223 183 170 213
   119 248 152   2  44 154 163  70 221 153 101 155 167  43 172   9
   129  22  39 253  19  98 108 110  79 113 224 232 178 185 112 104
   218 246  97 228 251  34 242 193 238 210 144  12 191 179 162 241
   81   51 145 235 249  14 239 107  49 192 214  31 181 199 106 157
   184  84 204 176 115 121  50  45 127   4 150 254 138 236 205  93
   222 114  67  29  24  72 243 141 128 195  78  66 215  61 156 180])

(defn perm-in [] (in perm-table 6))

(defn perm-get [] (get perm-table 6))

# Call types
(def my-123 (fn [] 123))
(def jitted-123 (jit/jitable my-123))

(defn call-fn []
  (my-123))

(defn call-cfun []
  (math/sin 123))

(defn call-jit-fun []
  (jitted-123))

(defn bubble-sort [list]
  (var sorted false)
  (while (not sorted)
    (set sorted true)
    (for i 0 (- (length list) 1)
      (def lhs (get list i))
      (def rhs (get list (+ i 1)))
      (if (> lhs rhs)
	(do
	  (set sorted false)
	  (put list i rhs)
	  (put list (+ i 1) lhs))))))

(defn copy-perm-table [] (array ;perm-table))

(defn bubble-sort-fresh []
  (bubble-sort (copy-perm-table)))

# Copy of deep-not= from boot.janet
(defn copy-deep-not=
  ``Like `not=`, but mutable types (arrays, tables, buffers) are considered
  equal if they have identical structure. Much slower than `not=`.``
  [x y]
  (def tx (type x))
  (or
    (not= tx (type y))
    (cond
      (or (= tx :tuple) (= tx :array))
      (or (not= (length x) (length y))
          (do
            (var ret false)
            (forv i 0 (length x)
              (def xx (in x i))
              (def yy (in y i))
              (if (deep-not= xx yy)
                (break (set ret true))))
            ret))
      (or (= tx :struct) (= tx :table))
      (or (not= (length x) (length y))
          (do
            (def rawget (if (= tx :struct) struct/rawget table/rawget))
            (var ret false)
            (eachp [k v] x
              (if (copy-deep-not= (rawget y k) v) (break (set ret true))))
            ret))
      (= tx :buffer) (not= 0 (- (length x) (length y)) (memcmp x y))
      (not= x y))))

(defn calculate-perlin-gradients
  []
  (def angle-count 16)
  (def angle-mul (* (/ math/pi angle-count) 2))
  (def dirs @[])
  (for i 0 angle-count
    (array/push dirs [(math/cos (* i angle-mul)) (math/sin (* i angle-mul))]))
  (var vecs @[])
  (for i 0 256
    (array/push vecs (get dirs (% i angle-count))))
  vecs)

(def deep-array [[1 2] [[1 2 3] [1 2 3]] [[[1 2 3 4]] [[1 2 3 4]]]])
(def deep-table { :first { :x 1 :y 2} :second { :second-first { :x 1 :y 2 :z 3} :second-second { :x 1 :y 2 :z 3}} :third { :deep{ :deeper { :r 1 :g 2 :b 3 :a 4}}}})

(print "---------------------------------------------------------------------------------------")
(print "| name | interpreter ips | interpreter elapsed | jit ips | jit elapsed | times faster |")
(print "| ---- | --------------- | ------------------- | ------- | ----------- | ------------ |")
(prin "| 10 mul ")
(jit-perf-cmp 100000000 (fn [x] (* x x x x x x x x x x)) 5)
# (prin "| len (tuple) ")
# (jit-perf-cmp 100000000 (fn [x] (length x)) [1 2 3])
# (prin "| len (string) ")
# (jit-perf-cmp 100000000 (fn [x] (length x)) "1 2 3")
# (prin "| len (buffer) ")
# (jit-perf-cmp 100000000 (fn [x] (length x)) @"hithere")
# (prin "| get (tuple & number) ")
# (jit-perf-cmp 100000000 (fn [col key] (get col key)) [1 2 3] 1)
# (prin "| get (tuple & number) ")
# (jit-perf-cmp 100000000 (fn [col key] (get col key)) [1 2 3] 1)
(prin "| lerp ")
(jit-perf-cmp 10000000 lerp 1 300 0.765)
(prin "| fade ")
(jit-perf-cmp 10000000 fade 20)
(prin "| dot  ")
(jit-perf-cmp 10000000 dot 20.234 30.567 1.23 9.87)
(prin "| bubble sort ")
(jit-perf-cmp 1000 bubble-sort-fresh)
# (prin "| perlin gradients ")
# (jit-perf-cmp 100000 calculate-perlin-gradients)
# (prin "| in   ")
# (jit-perf-cmp 10000000 perm-in)
# (prin "| get  ")
# (jit-perf-cmp 10000000 perm-get)
# (prin "| call  ")
# (jit-perf-cmp 10000000 call-fn)
# (prin "| C call  ")
# (jit-perf-cmp 10000000 call-cfun)
# (prin "| call jit  ")
# (jit-perf-cmp 1000000 call-jit-fun)
# (prin "| deep-not= (true)")
# (jit-perf-cmp 100000 copy-deep-not= { :x 123 :y 321 } { :x 123 :y 321 })
# (prin "| deep-not= (false type difference)")
# (jit-perf-cmp 100000 copy-deep-not= { :x 123 :y 321 } { :x 123 :y "cat" })
# (prin "| deep-not= (false value difference)")
# (jit-perf-cmp 100000 copy-deep-not= { :x 123 :y 321 } { :x 123 :y 123 })
# (prin "| deep-not= (deep array)")
# (jit-perf-cmp 100000 copy-deep-not= deep-array deep-array)
# (prin "| deep-not= (deep table)")
# (jit-perf-cmp 100000 copy-deep-not= deep-table deep-table)
# (prin "| cmp (numeric) ")
# (jit-perf-cmp 10000000 my-compare 10 20)
# (prin "| cmp (type difference) ")
# (jit-perf-cmp 10000000 my-compare 10 :hi)
# (prin "| cmp (not numeric) ")
# (jit-perf-cmp 1000000 my-compare :hi :hi)
