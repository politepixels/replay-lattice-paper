(* Copyright 2026 PolitePixels Limited

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. *)

Require Import ReplayLattice.
Require Import List Arith Lia.
Import ListNotations.

Record msched : Type := mkSched {
  smode    : nat;
  stile    : nat;
  sworkers : nat
}.

Section Kernels.

Variable A : Type.
Variable f : A -> A -> A.
Variable seed : A.

Fixpoint chain (acc : A) (l : list A) : A :=
  match l with
  | []      => acc
  | x :: r  => chain (f acc x) r
  end.

Fixpoint chunks (fuel k : nat) (l : list A) : list (list A) :=
  match fuel with
  | O => []
  | S fuel' =>
      match l with
      | [] => []
      | _  => firstn k l :: chunks fuel' k (skipn k l)
      end
  end.

Definition blocked (k : nat) (l : list A) : A :=
  chain seed (map (chain seed) (chunks (length l) k l)).

Definition EVAL_BLOCK : nat := 64.

Definition kernel_fixed_tree (s : msched) (l : list A) : A := blocked EVAL_BLOCK l.

Theorem row_fixed_tree_reduction :
  forall s s' l, kernel_fixed_tree s l = kernel_fixed_tree s' l.
Proof. reflexivity. Qed.

Definition kernel_tree_by_workers (s : msched) (l : list A) : A := blocked (S (sworkers s)) l.

Definition kernel_move (s : msched) (x : A) : A := x.

Theorem row_data_movement :
  forall s s' x, kernel_move s x = kernel_move s' x.
Proof. reflexivity. Qed.

Definition kernel_move_by_workers (s : msched) (x y : A) : A :=
  match sworkers s with O => x | S _ => y end.

Definition kernel_recurrence (s : msched) (l : list A) : A := chain seed l.

Theorem row_carried_recurrence :
  forall s s' l, kernel_recurrence s l = kernel_recurrence s' l.
Proof. reflexivity. Qed.

Definition kernel_recurrence_by_workers (s : msched) (l : list A) : A :=
  chain (match sworkers s with O => seed | S _ => f seed seed end) l.

Definition free_width (s : msched) (n : nat) : nat :=
  let lanes := Nat.max 1 (sworkers s) in Nat.max 1 ((n + lanes - 1) / lanes).

Definition kernel_free (s : msched) (l : list A) : A := blocked (free_width s (length l)) l.

Definition SCANW : nat := 64.

Fixpoint scan_from (acc : A) (l : list A) : list A :=
  match l with
  | []     => []
  | x :: r => let v := f acc x in v :: scan_from v r
  end.

Fixpoint scan_over (acc : A) (cs : list (list A)) : list A :=
  match cs with
  | []        => []
  | c :: rest => map (fun local => f acc local) (scan_from seed c)
                   ++ scan_over (f acc (chain seed c)) rest
  end.

Definition scan_blocked (k : nat) (l : list A) : list A :=
  scan_over seed (chunks (length l) k l).

Definition kernel_scan_fixed (s : msched) (l : list A) : list A := scan_blocked SCANW l.

Definition kernel_scan_by_workers (s : msched) (l : list A) : list A :=
  scan_blocked (free_width s (length l)) l.

Theorem row_fixed_block_scan :
  forall s s' l, kernel_scan_fixed s l = kernel_scan_fixed s' l.
Proof. reflexivity. Qed.

End Kernels.

Definition wf2 (a b : nat) : nat := 2 * a + b.

Theorem witness_scan_by_workers_moves :
  exists l s s',
    kernel_scan_by_workers nat wf2 0 s l <> kernel_scan_by_workers nat wf2 0 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 1), (mkSched 0 0 2).
  vm_compute. discriminate.
Qed.

Theorem witness_free_reduction_moves :
  exists l s s',
    kernel_free nat wf2 0 s l <> kernel_free nat wf2 0 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 1), (mkSched 0 0 2).
  vm_compute. discriminate.
Qed.

Theorem witness_tree_by_workers_moves :
  exists l s s',
    kernel_tree_by_workers nat wf2 0 s l <> kernel_tree_by_workers nat wf2 0 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute. discriminate.
Qed.

Theorem witness_move_by_workers_moves :
  exists x y s s',
    kernel_move_by_workers nat s x y <> kernel_move_by_workers nat s' x y.
Proof.
  exists 0, 1, (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute. discriminate.
Qed.

Theorem witness_recurrence_by_workers_moves :
  exists l s s',
    kernel_recurrence_by_workers nat wf2 1 s l <> kernel_recurrence_by_workers nat wf2 1 s' l.
Proof.
  exists [1; 1; 1; 1], (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute. discriminate.
Qed.

Theorem approx_rows_are_vacuous :
  forall (P : prog) i n s s' w,
    nth_error P i = Some n ->
    op_lane n = Approx ->
    agrees (op_lane n) s s' ->
    eval P i s w = eval P i s' w.
Proof.
  intros P i n s s' w _ Hop Hag.
  rewrite Hop in Hag. simpl in Hag. contradiction.
Qed.

Definition kernel_source (A : Type) (s : msched) (x : A) : A := x.

Theorem row_external_source_at_fixed_world :
  forall (A : Type) s s' (x : A), kernel_source A s x = kernel_source A s' x.
Proof. reflexivity. Qed.

Definition source_by_workers (s : msched) (x y : nat) : nat :=
  match sworkers s with O => x | S _ => y end.

Theorem witness_source_by_workers_moves :
  exists x y s s', source_by_workers s x y <> source_by_workers s' x y.
Proof.
  exists 0, 1, (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute. discriminate.
Qed.

Definition reference (A : Type) (f : A -> A -> A) (seed : A) (l : list A) : A :=
  chain A f seed l.

Definition kernel_oracle (A : Type) (f : A -> A -> A) (seed : A)
                         (s : msched) (l : list A) : A := chain A f seed l.

Theorem row_oracle_order_matches_reference :
  forall A f seed s l, kernel_oracle A f seed s l = reference A f seed l.
Proof. reflexivity. Qed.

Theorem witness_fixed_tree_is_not_reference :
  exists l, kernel_fixed_tree nat wf2 0 (mkSched 0 0 0) l <> reference nat wf2 0 l.
Proof.
  exists (repeat 0 63 ++ [1; 0; 0]). vm_compute. discriminate.
Qed.

Definition kernel_elementwise (A B : Type) (g : A -> B) (s : msched) (l : list A) : list B :=
  map g l.

Theorem row_elementwise :
  forall A B (g : A -> B) s s' l,
    kernel_elementwise A B g s l = kernel_elementwise A B g s' l.
Proof. reflexivity. Qed.

Definition elementwise_by_workers (s : msched) (l : list nat) : list nat :=
  map (fun x => match sworkers s with O => x | S _ => S x end) l.

Theorem witness_elementwise_by_workers_moves :
  exists l s s', elementwise_by_workers s l <> elementwise_by_workers s' l.
Proof.
  exists [0], (mkSched 0 0 0), (mkSched 0 0 1).
  vm_compute. discriminate.
Qed.

Section ExactInteger.

Variable A : Type.
Variable g : A -> A -> A.
Variable e : A.
Variable dom : A -> Prop.
Hypothesis dom_e : dom e.
Hypothesis dom_closed : forall a b, dom a -> dom b -> dom (g a b).
Hypothesis g_assoc : forall a b c, dom a -> dom b -> dom c -> g (g a b) c = g a (g b c).
Hypothesis g_left  : forall a, g e a = a.
Hypothesis g_right : forall a, g a e = a.

Lemma dom_chain : forall l acc, dom acc -> Forall dom l -> dom (chain A g acc l).
Proof.
  induction l as [| x r IH]; intros acc Hacc Hl; simpl; [ exact Hacc | ].
  inversion Hl as [| ? ? Hx Hr]; subst.
  apply IH; [ apply dom_closed; assumption | assumption ].
Qed.

Lemma chain_acc : forall l acc,
  dom acc -> Forall dom l -> chain A g acc l = g acc (chain A g e l).
Proof.
  induction l as [| x r IH]; intros acc Hacc Hl; simpl.
  - rewrite g_right. reflexivity.
  - inversion Hl as [| ? ? Hx Hr]; subst.
    rewrite (IH (g acc x) (dom_closed _ _ Hacc Hx) Hr).
    rewrite (IH (g e x) (dom_closed _ _ dom_e Hx) Hr).
    rewrite g_left.
    rewrite g_assoc; [ reflexivity | assumption | assumption | ].
    apply dom_chain; [ exact dom_e | exact Hr ].
Qed.

Lemma chain_app : forall l1 l2,
  Forall dom l1 -> Forall dom l2 ->
  chain A g e (l1 ++ l2) = g (chain A g e l1) (chain A g e l2).
Proof.
  intros l1 l2 H1 H2.
  assert (Hsplit : forall l1' acc, chain A g acc (l1' ++ l2) = chain A g (chain A g acc l1') l2).
  { induction l1' as [| x r IH]; intros acc; simpl; [ reflexivity | apply IH ]. }
  rewrite Hsplit, (chain_acc l2 (chain A g e l1)); [ reflexivity | | exact H2 ].
  apply dom_chain; [ exact dom_e | exact H1 ].
Qed.

Lemma chunks_concat : forall fuel k l,
  1 <= k -> length l <= fuel -> concat (chunks A fuel k l) = l.
Proof.
  induction fuel as [| fuel IH]; intros k l Hk Hlen; simpl.
  - destruct l; [ reflexivity | simpl in Hlen; lia ].
  - destruct l as [| x r]; [ reflexivity | ].
    simpl. rewrite IH.
    + apply firstn_skipn.
    + exact Hk.
    +
      rewrite length_skipn. lia.
Qed.

Lemma forall_firstn : forall k (l : list A), Forall dom l -> Forall dom (firstn k l).
Proof.
  induction k as [| k IH]; intros l Hl; simpl; [ constructor | ].
  destruct l as [| x r]; [ constructor | ].
  inversion Hl as [| ? ? Hx Hr]; subst.
  constructor; [ exact Hx | apply IH; exact Hr ].
Qed.

Lemma forall_skipn : forall k (l : list A), Forall dom l -> Forall dom (skipn k l).
Proof.
  induction k as [| k IH]; intros l Hl; simpl; [ exact Hl | ].
  destruct l as [| x r]; [ constructor | ].
  inversion Hl as [| ? ? Hx Hr]; subst. apply IH; exact Hr.
Qed.

Lemma chunks_dom : forall fuel k l,
  Forall dom l -> Forall (Forall dom) (chunks A fuel k l).
Proof.
  induction fuel as [| fuel IH]; intros k l Hl; simpl; [ constructor | ].
  destruct l as [| x r]; [ constructor | ].
  constructor.
  - apply forall_firstn. exact Hl.
  - apply IH. apply forall_skipn. exact Hl.
Qed.

Lemma concat_dom : forall ls, Forall (Forall dom) ls -> Forall dom (concat ls).
Proof.
  induction ls as [| c cs IH]; intros H; simpl; [ constructor | ].
  inversion H as [| ? ? Hc Hcs]; subst.
  apply Forall_app. split; [ exact Hc | apply IH; exact Hcs ].
Qed.

Lemma chain_map_chunks : forall ls,
  Forall (Forall dom) ls ->
  chain A g e (map (chain A g e) ls) = chain A g e (concat ls).
Proof.
  induction ls as [| c cs IH]; intros H; simpl; [ reflexivity | ].
  inversion H as [| ? ? Hc Hcs]; subst.
  rewrite chain_app by (exact Hc || apply concat_dom, Hcs).
  rewrite <- IH by exact Hcs.
  rewrite (chain_acc (map (chain A g e) cs) (g e (chain A g e c))).
  - rewrite g_left. reflexivity.
  - apply dom_closed; [ exact dom_e | apply dom_chain; [ exact dom_e | exact Hc ] ].
  - apply Forall_map. apply Forall_impl with (P := Forall dom); [ | exact Hcs ].
    intros a Ha. apply dom_chain; [ exact dom_e | exact Ha ].
Qed.

Theorem row_exact_integer :
  forall k l, 1 <= k -> Forall dom l -> blocked A g e k l = chain A g e l.
Proof.
  intros k l Hk Hl. unfold blocked.
  rewrite chain_map_chunks by (apply chunks_dom; exact Hl).
  rewrite chunks_concat by (assumption || apply Nat.le_refl).
  reflexivity.
Qed.

Corollary row_exact_integer_schedule_invariant :
  forall s s' l, Forall dom l -> kernel_free A g e s l = kernel_free A g e s' l.
Proof.
  intros s s' l Hl. unfold kernel_free.
  rewrite !row_exact_integer by (apply Nat.le_max_l || exact Hl). reflexivity.
Qed.

End ExactInteger.

Require Import ZArith.
Require Import Floats.

Theorem row_exact_integer_at_Z :
  forall s s' (l : list Z), kernel_free Z Z.add 0%Z s l = kernel_free Z Z.add 0%Z s' l.
Proof.
  intros s s' l.
  apply (row_exact_integer_schedule_invariant Z Z.add 0%Z (fun _ => True));
    try (intros; exact I); try (intros; lia).
  apply Forall_forall. intros x _. exact I.
Qed.

Theorem witness_float_reassociation_moves :
  ((0.1 + 0.2) + 0.3 =? 0.1 + (0.2 + 0.3))%float = false.
Proof. vm_compute. reflexivity. Qed.

Definition mworld := nat.

Definition mbits := nat.

Definition ORACLE_MODE : nat := 3.

Definition mgated (s : msched) : Prop := smode s <> ORACLE_MODE.

Definition magrees (l : lane) (s s' : msched) : Prop :=
  match l with
  | Exact  => True
  | Replay => mgated s /\ mgated s'
  | Approx => False
  end.

Fixpoint meval_at (fuel : nat) (P : prog) (i : nat) (s : msched) (w : mworld) : mbits :=
  match fuel with
  | O => w
  | S fuel' =>
      match nth_error P i with
      | None => w
      | Some n =>
          let operands := map (fun j => meval_at fuel' P j s w) (inputs n) in
          let carried_value :=
            match carried n with
            | Some c => meval_at fuel' P c s w
            | None   => 0
            end in
          match op_lane n with
          | Exact  => chain nat Nat.add w (carried_value :: operands)
          | Replay => if Nat.eqb (smode s) ORACLE_MODE then chain nat Nat.add w (carried_value :: operands)
                      else kernel_fixed_tree nat Nat.add w s (carried_value :: operands)
          | Approx => if Nat.eqb (smode s) ORACLE_MODE then chain nat Nat.add w (carried_value :: operands)
                      else kernel_free nat Nat.add w s (carried_value :: operands)
          end
      end
  end.

Definition meval (P : prog) (i : nat) (s : msched) (w : mworld) : mbits :=
  meval_at (S i) P i s w.

Definition demo_prog : prog :=
  [ mkNode Exact  []      None         true  None
  ; mkNode Replay [0]     (Some Replay) false None
  ; mkNode Approx [0; 1]  None         false None ].

Definition node_inputs_of (o : option node) : list nat :=
  match o with Some x => inputs x | None => [] end.

Definition node_lane_of (o : option node) : lane :=
  match o with Some x => op_lane x | None => Exact end.

Theorem demo_prog_wf : wf demo_prog.
Proof.
  intros i n j Hn Hj.
  apply (f_equal node_inputs_of) in Hn.
  unfold demo_prog in Hn.
  destruct i as [| [| [| i]]]; cbn in Hn; rewrite <- Hn in Hj; cbn in Hj.
  - destruct Hj.
  - destruct Hj as [<- | []]; lia.
  - destruct Hj as [<- | [<- | []]]; lia.
  - destruct i; cbn in Hj; destruct Hj.
Qed.

Theorem demo_assumption1 :
  forall i n s s' w,
    nth_error demo_prog i = Some n ->
    magrees (op_lane n) s s' ->
    (forall j, In j (inputs n) -> meval demo_prog j s w = meval demo_prog j s' w) ->
    (forall c, carried n = Some c -> meval demo_prog c s w = meval demo_prog c s' w) ->
    meval demo_prog i s w = meval demo_prog i s' w.
Proof.
  intros i n s s' w Hn Hag Hin Hcar.
  assert (Hlane := f_equal node_lane_of Hn).
  unfold demo_prog in Hlane.
  destruct i as [| [| [| i]]]; cbn in Hlane.
  - reflexivity.
  - rewrite <- Hlane in Hag. destruct Hag as [Hg Hg'].
    apply Nat.eqb_neq in Hg, Hg'.
    unfold meval. cbn [meval_at nth_error demo_prog op_lane inputs carried map].
    rewrite Hg, Hg'. reflexivity.
  - rewrite <- Hlane in Hag. cbn in Hag. contradiction.
  - destruct i; unfold meval; cbn; reflexivity.
Qed.

Theorem mgated_separates :
  (exists s, mgated s) /\ (exists s, ~ mgated s).
Proof.
  split; [ exists (mkSched 0 0 0) | exists (mkSched ORACLE_MODE 0 0) ]; unfold mgated; simpl.
  - unfold ORACLE_MODE. discriminate.
  - intros H. apply H. reflexivity.
Qed.
